/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for kpcand.c on synthetic samples (no biometric data). */
#include "kpcand.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                              \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

static const double PI = 3.14159265358979323846;

static double uniform(unsigned *s)
{
  *s = *s * 1103515245u + 12345u;
  return (double)((*s >> 8) & 0xffffu) / 65535.0;
}

static void finger(unsigned seed, int ox, int oy, unsigned noise, int32_t out[KPC_PIXELS])
{
  double f[24], a[24], p[24];
  for (int i = 0; i < 24; i++) { f[i] = 0.11 + 0.05 * uniform(&seed); a[i] = PI * uniform(&seed); p[i] = 2 * PI * uniform(&seed); }
  for (int r = 0; r < KPC_ROWS; r++)
    for (int c = 0; c < KPC_COLS; c++) {
      double v = 0;
      for (int i = 0; i < 24; i++) v += cos(2 * PI * f[i] * ((c + ox) * cos(a[i]) + (r + oy) * sin(a[i])) + p[i]);
      out[r * KPC_COLS + c] = (int32_t)lround(20000 + 3000 * v / sqrt(24.0) + 150 * (uniform(&noise) - 0.5));
    }
}

static void test_prepare_scaling_and_outlier(void)
{
  static int32_t cur[KPC_PIXELS];
  static double a[KPC_PIXELS], b[KPC_PIXELS];
  finger(101u, 20, 20, 5u, cur);
  CHECK(kpc_prepare(cur, NULL, a) == 0, "ridged image scorable");
  double lo = 1, hi = 0;
  for (int i = 0; i < KPC_PIXELS; i++) { lo = fmin(lo, a[i]); hi = fmax(hi, a[i]); }
  CHECK(lo == 0.0 && hi == 1.0, "scaled and clipped onto [0, 1]: %g..%g", lo, hi);
  /* one extreme pixel must not compress the rest (min/max would) */
  cur[500] = 2000000;
  CHECK(kpc_prepare(cur, NULL, b) == 0, "outlier image scorable");
  int same = 0;
  for (int i = 0; i < KPC_PIXELS; i++) same += (i != 500 && fabs(a[i] - b[i]) < 1e-3);
  CHECK(same > KPC_PIXELS - 50, "outlier changed %d other pixels", KPC_PIXELS - 1 - same);
}

static void test_prepare_unscorable(void)
{
  static int32_t flat[KPC_PIXELS];
  static double out[KPC_PIXELS];
  for (int i = 0; i < KPC_PIXELS; i++) flat[i] = 20000;
  CHECK(kpc_prepare(flat, NULL, out) == -1, "flat image unscorable");
  for (int i = 0; i < KPC_PIXELS; i++) flat[i] = (i < 60) ? 30000 : 20000; /* std >= 1 but p1 == p99 */
  CHECK(kpc_prepare(flat, NULL, out) == -1, "collapsed percentile range unscorable");
}

static void test_reference_subtraction(void)
{
  static int32_t cur[KPC_PIXELS], ref[KPC_PIXELS];
  static double a[KPC_PIXELS], b[KPC_PIXELS];
  finger(101u, 20, 20, 5u, cur);
  for (int i = 0; i < KPC_PIXELS; i++) ref[i] = 20000;
  kpc_prepare(cur, NULL, a);
  kpc_prepare(cur, ref, b);
  double err = 0;
  for (int i = 0; i < KPC_PIXELS; i++) err = fmax(err, fabs(a[i] - b[i]));
  CHECK(err < 1e-12, "constant reference changes nothing after scaling: %g", err);
}

static void test_compare_genuine_impostor_and_no_match(void)
{
  static int32_t m[KPC_PIXELS], p[KPC_PIXELS], o[KPC_PIXELS];
  static double im[KPC_PIXELS], ip[KPC_PIXELS], io[KPC_PIXELS];
  finger(101u, 20, 20, 5u, m);
  finger(101u, 25, 17, 6u, p); /* m = p + (5, -3) */
  finger(202u, 20, 20, 7u, o);
  kpc_prepare(m, NULL, im); kpc_prepare(p, NULL, ip); kpc_prepare(o, NULL, io);
  KpSet sm, sp, so;
  CHECK(kpc_extract(im, &sm) == 0 && kpc_extract(ip, &sp) == 0 && kpc_extract(io, &so) == 0, "extraction");
  KpcResult g = kpc_compare(&sp, &sm), i = kpc_compare(&so, &sm);
  CHECK(g.registered && g.score >= 5 && g.angle == 0 && abs(g.dx - 5) <= 1 && abs(g.dy + 3) <= 1,
        "genuine: score %d angle %d d (%d, %d)", g.score, g.angle, g.dx, g.dy);
  CHECK(g.score > 2 * i.score, "genuine %d vs impostor %d", g.score, i.score);
  KpSet empty = {NULL, 0};
  KpcResult z = kpc_compare(&sp, &empty);
  CHECK(z.score == 0 && !z.registered, "nothing to match: no-match");
  kpc_free(&sm); kpc_free(&sp); kpc_free(&so);
}

int main(void)
{
  test_prepare_scaling_and_outlier();
  test_prepare_unscorable();
  test_reference_subtraction();
  test_compare_genuine_impostor_and_no_match();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
