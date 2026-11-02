/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* SIFT per Rey-Otero and Delbracio, "Anatomy of the SIFT Method", IPOL 4
 * (2014). Algorithm numbers below refer to that article. Internal storage is
 * row-major with m = row and n = column; IPOL's x is the row coordinate and y
 * the column coordinate. Keypoints are returned as (x = column, y = row). */
#include "sift.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define TWO_PI 6.28318530717958647692
#define N_IMG (SIFT_N_SPO + 3) /* v_0 .. v_{n_spo+2} */
#define N_DOG (SIFT_N_SPO + 2) /* w_0 .. w_{n_spo+1} */
#define N_FEAT (SIFT_N_HIST * SIFT_N_HIST * SIFT_N_ORI)

typedef struct {
  int M, N;
  double delta;
  double *v[N_IMG], *w[N_DOG], *gm[N_IMG], *gn[N_IMG];
} Octave;

int sift_octaves(int rows, int cols)
{
  int mn = rows < cols ? rows : cols;
  return (int)floor(log2((double)mn / SIFT_DELTA_MIN / 12.0) + 1.0);
}

/* symmetric extension with respect to -1/2: s_M(k) = min(k mod 2M, 2M-1-k mod 2M) */
static int sym(int k, int M)
{
  int p = 2 * M;
  k %= p;
  if (k < 0) k += p;
  return k < M ? k : p - 1 - k;
}

/* digital Gaussian smoothing, kernel truncated at ceil(4 sigma), separable (eq. 3-4) */
static int blur(const double *in, double *out, int M, int N, double sigma)
{
  int K = (int)ceil(4.0 * sigma);
  double *g = malloc(sizeof *g * (size_t)(2 * K + 1)), *tmp = malloc(sizeof *tmp * (size_t)M * (size_t)N);
  if (!g || !tmp) { free(g); free(tmp); return -1; }
  double sum = 0.0;
  for (int k = -K; k <= K; k++) sum += g[k + K] = exp(-(double)(k * k) / (2.0 * sigma * sigma));
  for (int k = 0; k <= 2 * K; k++) g[k] /= sum;
  for (int m = 0; m < M; m++)
    for (int n = 0; n < N; n++) {
      double acc = 0.0;
      for (int k = -K; k <= K; k++) acc += g[k + K] * in[m * N + sym(n - k, N)];
      tmp[m * N + n] = acc;
    }
  for (int m = 0; m < M; m++)
    for (int n = 0; n < N; n++) {
      double acc = 0.0;
      for (int k = -K; k <= K; k++) acc += g[k + K] * tmp[sym(m - k, M) * N + n];
      out[m * N + n] = acc;
    }
  free(g);
  free(tmp);
  return 0;
}

/* Algorithm 2: bilinear interpolation to inter-pixel distance d (< 1) */
static void upsample(const double *u, int M, int N, double d, double *out, int Mo, int No)
{
  for (int mo = 0; mo < Mo; mo++)
    for (int no = 0; no < No; no++) {
      double x = d * mo, y = d * no;
      int fx = (int)floor(x), fy = (int)floor(y);
      double ax = x - fx, ay = y - fy;
      out[mo * No + no] = ax * ay * u[sym(fx + 1, M) * N + sym(fy + 1, N)] +
                          (1.0 - ax) * ay * u[sym(fx, M) * N + sym(fy + 1, N)] +
                          ax * (1.0 - ay) * u[sym(fx + 1, M) * N + sym(fy, N)] +
                          (1.0 - ax) * (1.0 - ay) * u[sym(fx, M) * N + sym(fy, N)];
    }
}

static void free_octaves(Octave *oct, int n)
{
  for (int o = 0; o < n; o++) {
    for (int s = 0; s < N_IMG; s++) { free(oct[o].v[s]); free(oct[o].gm[s]); free(oct[o].gn[s]); }
    for (int s = 0; s < N_DOG; s++) free(oct[o].w[s]);
  }
}

/* Algorithms 1, 3 and 10: scale-space, DoG and gradients */
static int build(const double *image, int rows, int cols, Octave *oct, int n_oct)
{
  memset(oct, 0, sizeof *oct * (size_t)n_oct);
  for (int o = 0; o < n_oct; o++) {
    Octave *c = &oct[o];
    c->M = o == 0 ? (int)floor(rows / SIFT_DELTA_MIN) : oct[o - 1].M / 2;
    c->N = o == 0 ? (int)floor(cols / SIFT_DELTA_MIN) : oct[o - 1].N / 2;
    c->delta = SIFT_DELTA_MIN * pow(2.0, o);
    size_t size = (size_t)c->M * (size_t)c->N;
    for (int s = 0; s < N_IMG; s++) {
      c->v[s] = calloc(size, sizeof(double));
      c->gm[s] = calloc(size, sizeof(double));
      c->gn[s] = calloc(size, sizeof(double));
      if (!c->v[s] || !c->gm[s] || !c->gn[s]) return -1;
    }
    for (int s = 0; s < N_DOG; s++)
      if (!(c->w[s] = calloc(size, sizeof(double)))) return -1;
    if (o == 0) {
      double *up = malloc(sizeof *up * size);
      if (!up) return -1;
      upsample(image, rows, cols, SIFT_DELTA_MIN, up, c->M, c->N);
      double seed = sqrt(SIFT_SIGMA_MIN * SIFT_SIGMA_MIN - SIFT_SIGMA_IN * SIFT_SIGMA_IN) / SIFT_DELTA_MIN;
      int rc = blur(up, c->v[0], c->M, c->N, seed);
      free(up);
      if (rc) return -1;
    } else {
      const Octave *p = &oct[o - 1];
      for (int m = 0; m < c->M; m++)
        for (int n = 0; n < c->N; n++) c->v[0][m * c->N + n] = p->v[SIFT_N_SPO][(2 * m) * p->N + 2 * n];
    }
    for (int s = 1; s < N_IMG; s++) {
      double rho = (SIFT_SIGMA_MIN / SIFT_DELTA_MIN) *
                   sqrt(pow(2.0, 2.0 * s / SIFT_N_SPO) - pow(2.0, 2.0 * (s - 1) / SIFT_N_SPO));
      if (blur(c->v[s - 1], c->v[s], c->M, c->N, rho)) return -1;
    }
    for (int s = 0; s < N_DOG; s++)
      for (size_t i = 0; i < size; i++) c->w[s][i] = c->v[s + 1][i] - c->v[s][i];
    for (int s = 1; s <= SIFT_N_SPO; s++)
      for (int m = 1; m < c->M - 1; m++)
        for (int n = 1; n < c->N - 1; n++) {
          c->gm[s][m * c->N + n] = (c->v[s][(m + 1) * c->N + n] - c->v[s][(m - 1) * c->N + n]) / 2.0;
          c->gn[s][m * c->N + n] = (c->v[s][m * c->N + n + 1] - c->v[s][m * c->N + n - 1]) / 2.0;
        }
  }
  return 0;
}

static int is_extremum(const Octave *c, int s, int m, int n)
{
  double v = c->w[s][m * c->N + n];
  int greater = 1, smaller = 1;
  for (int ds = -1; ds <= 1; ds++)
    for (int dm = -1; dm <= 1; dm++)
      for (int dn = -1; dn <= 1; dn++) {
        if (!ds && !dm && !dn) continue;
        double u = c->w[s + ds][(m + dm) * c->N + n + dn];
        if (u >= v) greater = 0;
        if (u <= v) smaller = 0;
      }
  return greater || smaller;
}

/* Algorithm 7 helpers: 3D gradient and Hessian (eq. 13), index order (s, m, n) */
static void derivatives(const Octave *c, int s, int m, int n, double g[3], double H[3][3])
{
  const int N = c->N;
#define W(ss, mm, nn) c->w[ss][(mm) * N + (nn)]
  double v = W(s, m, n);
  g[0] = (W(s + 1, m, n) - W(s - 1, m, n)) / 2.0;
  g[1] = (W(s, m + 1, n) - W(s, m - 1, n)) / 2.0;
  g[2] = (W(s, m, n + 1) - W(s, m, n - 1)) / 2.0;
  H[0][0] = W(s + 1, m, n) + W(s - 1, m, n) - 2.0 * v;
  H[1][1] = W(s, m + 1, n) + W(s, m - 1, n) - 2.0 * v;
  H[2][2] = W(s, m, n + 1) + W(s, m, n - 1) - 2.0 * v;
  H[0][1] = H[1][0] = (W(s + 1, m + 1, n) - W(s + 1, m - 1, n) - W(s - 1, m + 1, n) + W(s - 1, m - 1, n)) / 4.0;
  H[0][2] = H[2][0] = (W(s + 1, m, n + 1) - W(s + 1, m, n - 1) - W(s - 1, m, n + 1) + W(s - 1, m, n - 1)) / 4.0;
  H[1][2] = H[2][1] = (W(s, m + 1, n + 1) - W(s, m + 1, n - 1) - W(s, m - 1, n + 1) + W(s, m - 1, n - 1)) / 4.0;
#undef W
}

static int solve3(double H[3][3], const double g[3], double a[3])
{
  double det = H[0][0] * (H[1][1] * H[2][2] - H[1][2] * H[2][1]) -
               H[0][1] * (H[1][0] * H[2][2] - H[1][2] * H[2][0]) +
               H[0][2] * (H[1][0] * H[2][1] - H[1][1] * H[2][0]);
  if (fabs(det) < 1e-18) return -1;
  double inv[3][3];
  inv[0][0] = (H[1][1] * H[2][2] - H[1][2] * H[2][1]) / det;
  inv[0][1] = (H[0][2] * H[2][1] - H[0][1] * H[2][2]) / det;
  inv[0][2] = (H[0][1] * H[1][2] - H[0][2] * H[1][1]) / det;
  inv[1][0] = (H[1][2] * H[2][0] - H[1][0] * H[2][2]) / det;
  inv[1][1] = (H[0][0] * H[2][2] - H[0][2] * H[2][0]) / det;
  inv[1][2] = (H[0][2] * H[1][0] - H[0][0] * H[1][2]) / det;
  inv[2][0] = (H[1][0] * H[2][1] - H[1][1] * H[2][0]) / det;
  inv[2][1] = (H[0][1] * H[2][0] - H[0][0] * H[2][1]) / det;
  inv[2][2] = (H[0][0] * H[1][1] - H[0][1] * H[1][0]) / det;
  for (int i = 0; i < 3; i++) a[i] = -(inv[i][0] * g[0] + inv[i][1] * g[1] + inv[i][2] * g[2]);
  return 0;
}

static double circ(double a)
{
  a = fmod(a, TWO_PI);
  return a < 0 ? a + TWO_PI : a;
}

typedef struct {
  Keypoint *k;
  int n, cap;
} List;

static int push(List *l, const Keypoint *k)
{
  if (l->n == l->cap) {
    int cap = l->cap ? 2 * l->cap : 64;
    Keypoint *nk = realloc(l->k, sizeof *nk * (size_t)cap);
    if (!nk) return -1;
    l->k = nk;
    l->cap = cap;
  }
  l->k[l->n++] = *k;
  return 0;
}

/* Algorithm 12 with the oriented border check; returns 0 if described */
static int describe(const Octave *c, int s, double x, double y, double sigma, double theta, int rows, int cols,
                    unsigned char out[KP_DESCRIPTOR])
{
  (void)rows; (void)cols;
  const double lam = SIFT_LAMBDA_DESCR, half = lam * sigma * (SIFT_N_HIST + 1) / SIFT_N_HIST;
  const double ct = cos(theta), st = sin(theta), d = c->delta;
  /* the oriented patch's corners must lie where gradients exist: [delta, (M-2) delta] */
  for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
      double r = x + half * (a * ct - b * st), q = y + half * (a * st + b * ct);
      if (r < d || q < d || r > (c->M - 2) * d || q > (c->N - 2) * d) return -1;
    }
  double hist[N_FEAT] = {0};
  double reach = half * sqrt(2.0);
  int m0 = (int)ceil((x - reach) / d), m1 = (int)floor((x + reach) / d);
  int n0 = (int)ceil((y - reach) / d), n1 = (int)floor((y + reach) / d);
  if (m0 < 1) m0 = 1;
  if (n0 < 1) n0 = 1;
  if (m1 > c->M - 2) m1 = c->M - 2;
  if (n1 > c->N - 2) n1 = c->N - 2;
  const double bin_width = 2.0 * lam / SIFT_N_HIST;
  for (int m = m0; m <= m1; m++)
    for (int n = n0; n <= n1; n++) {
      double dx = m * d - x, dy = n * d - y;
      double xh = (dx * ct + dy * st) / sigma, yh = (-dx * st + dy * ct) / sigma;
      if (fmax(fabs(xh), fabs(yh)) >= lam * (SIFT_N_HIST + 1) / SIFT_N_HIST) continue;
      double gm = c->gm[s][m * c->N + n], gn = c->gn[s][m * c->N + n];
      double th = circ(atan2(gn, gm) - theta); /* IPOL arctan2(dm, dn): angle of (dm, dn) */
      double w = exp(-(dx * dx + dy * dy) / (2.0 * (lam * sigma) * (lam * sigma))) * hypot(gm, gn);
      for (int i = 0; i < SIFT_N_HIST; i++) {
        double xi = (i + 1 - (1.0 + SIFT_N_HIST) / 2.0) * bin_width;
        if (fabs(xi - xh) > bin_width) continue;
        double wi = 1.0 - fabs(xh - xi) / bin_width;
        for (int j = 0; j < SIFT_N_HIST; j++) {
          double yj = (j + 1 - (1.0 + SIFT_N_HIST) / 2.0) * bin_width;
          if (fabs(yj - yh) > bin_width) continue;
          double wj = 1.0 - fabs(yh - yj) / bin_width;
          for (int k = 0; k < SIFT_N_ORI; k++) {
            double dt = fabs(th - TWO_PI * k / SIFT_N_ORI);
            dt = fmin(dt, TWO_PI - dt);
            if (dt >= TWO_PI / SIFT_N_ORI) continue;
            hist[(i * SIFT_N_HIST + j) * SIFT_N_ORI + k] += wi * wj * (1.0 - dt * SIFT_N_ORI / TWO_PI) * w;
          }
        }
      }
    }
  double norm = 0.0;
  for (int l = 0; l < N_FEAT; l++) norm += hist[l] * hist[l];
  norm = sqrt(norm);
  if (norm <= 0.0) return -1;
  for (int l = 0; l < N_FEAT; l++) hist[l] = fmin(hist[l], 0.2 * norm);
  norm = 0.0;
  for (int l = 0; l < N_FEAT; l++) norm += hist[l] * hist[l];
  norm = sqrt(norm);
  if (norm <= 0.0) return -1;
  for (int l = 0; l < N_FEAT; l++) {
    double v = floor(512.0 * hist[l] / norm);
    out[l] = (unsigned char)(v > 255.0 ? 255.0 : v);
  }
  return 0;
}

/* Algorithm 11: reference orientations; describes each and appends */
static int orient_and_describe(const Octave *c, int s, double x, double y, double sigma, int rows, int cols,
                               List *list)
{
  const double lam = SIFT_LAMBDA_ORI, d = c->delta;
  if (!(3 * lam * sigma <= x && x <= rows - 3 * lam * sigma && 3 * lam * sigma <= y && y <= cols - 3 * lam * sigma))
    return 0;
  double h[SIFT_N_BINS] = {0};
  int m0 = (int)lround((x - 3 * lam * sigma) / d), m1 = (int)lround((x + 3 * lam * sigma) / d);
  int n0 = (int)lround((y - 3 * lam * sigma) / d), n1 = (int)lround((y + 3 * lam * sigma) / d);
  for (int m = m0; m <= m1; m++)
    for (int n = n0; n <= n1; n++) {
      if (m < 1 || n < 1 || m > c->M - 2 || n > c->N - 2) continue;
      double gm = c->gm[s][m * c->N + n], gn = c->gn[s][m * c->N + n];
      double dx = m * d - x, dy = n * d - y;
      double w = exp(-(dx * dx + dy * dy) / (2.0 * (lam * sigma) * (lam * sigma))) * hypot(gm, gn);
      /* IPOL's arctan2(dm v, dn v) is the angle of the vector (dm, dn), i.e. C atan2(dn, dm) */
      int bin = (int)lround(SIFT_N_BINS / TWO_PI * circ(atan2(gn, gm))) % SIFT_N_BINS;
      h[bin] += w;
    }
  for (int it = 0; it < 6; it++) {
    double t[SIFT_N_BINS];
    for (int k = 0; k < SIFT_N_BINS; k++)
      t[k] = (h[(k + SIFT_N_BINS - 1) % SIFT_N_BINS] + h[k] + h[(k + 1) % SIFT_N_BINS]) / 3.0;
    memcpy(h, t, sizeof h);
  }
  double hmax = 0.0;
  for (int k = 0; k < SIFT_N_BINS; k++) hmax = fmax(hmax, h[k]);
  if (hmax <= 0.0) return 0;
  for (int k = 0; k < SIFT_N_BINS; k++) {
    double hm = h[(k + SIFT_N_BINS - 1) % SIFT_N_BINS], hp = h[(k + 1) % SIFT_N_BINS];
    if (!(h[k] > hm && h[k] > hp && h[k] >= SIFT_T * hmax)) continue;
    double theta = circ(TWO_PI * k / SIFT_N_BINS + (PI / SIFT_N_BINS) * (hm - hp) / (hm - 2.0 * h[k] + hp));
    Keypoint kp;
    memset(&kp, 0, sizeof kp);
    if (describe(c, s, x, y, sigma, theta, rows, cols, kp.d) != 0) continue;
    kp.x = y; /* column */
    kp.y = x; /* row */
    kp.sigma = sigma;
    kp.theta = theta;
    if (push(list, &kp)) return -1;
  }
  return 0;
}

int sift_detect(const double *image, int rows, int cols, Keypoint **out)
{
  *out = NULL;
  int n_oct = sift_octaves(rows, cols);
  if (n_oct < 1) return 0;
  Octave *oct = calloc((size_t)n_oct, sizeof *oct);
  if (!oct) return -1;
  List list = {NULL, 0, 0};
  int rc = build(image, rows, cols, oct, n_oct);
  const double edge_limit = (SIFT_C_EDGE + 1.0) * (SIFT_C_EDGE + 1.0) / SIFT_C_EDGE;
  for (int o = 0; rc == 0 && o < n_oct; o++) {
    const Octave *c = &oct[o];
    for (int s = 1; rc == 0 && s <= SIFT_N_SPO; s++)
      for (int m = 1; rc == 0 && m < c->M - 1; m++)
        for (int n = 1; rc == 0 && n < c->N - 1; n++) {
          if (fabs(c->w[s][m * c->N + n]) < 0.8 * SIFT_C_DOG) continue; /* Algorithm 5 */
          if (!is_extremum(c, s, m, n)) continue;                        /* Algorithm 4 */
          int ss = s, mm = m, nn = n, ok = 0;
          double a[3] = {0, 0, 0}, g[3], H[3][3], omega = 0.0;
          for (int it = 0; it < 5; it++) { /* Algorithm 6 */
            derivatives(c, ss, mm, nn, g, H);
            if (solve3(H, g, a)) break;
            if (fmax(fabs(a[0]), fmax(fabs(a[1]), fabs(a[2]))) < 0.6) {
              omega = c->w[ss][mm * c->N + nn] + 0.5 * (g[0] * a[0] + g[1] * a[1] + g[2] * a[2]);
              ok = 1;
              break;
            }
            ss += (int)lround(a[0]);
            mm += (int)lround(a[1]);
            nn += (int)lround(a[2]);
            if (ss < 1 || ss > SIFT_N_SPO || mm < 1 || mm > c->M - 2 || nn < 1 || nn > c->N - 2) break;
          }
          if (!ok || fabs(omega) < SIFT_C_DOG) continue; /* Algorithm 8 */
          derivatives(c, ss, mm, nn, g, H);             /* Algorithm 9: spatial Hessian */
          double tr = H[1][1] + H[2][2], det = H[1][1] * H[2][2] - H[1][2] * H[1][2];
          if (det <= 0.0 || tr * tr / det >= edge_limit) continue;
          double sigma = (c->delta / SIFT_DELTA_MIN) * SIFT_SIGMA_MIN * pow(2.0, (a[0] + ss) / SIFT_N_SPO);
          double x = c->delta * (a[1] + mm), y = c->delta * (a[2] + nn);
          rc = orient_and_describe(c, ss, x, y, sigma, rows, cols, &list);
        }
  }
  free_octaves(oct, n_oct);
  free(oct);
  if (rc) {
    free(list.k);
    return -1;
  }
  *out = list.k;
  return list.n;
}
