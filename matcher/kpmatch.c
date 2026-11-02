/*
 * SPDX-License-Identifier: LGPL-2.1-or-later AND MIT
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 * Copyright (c) 2021 Matthieu CHARETTE
 * Copyright (c) 2022 Goodix Fingerprint Linux Development
 *
 * kp_verify's per-anchor geometric verification is ported from SIGFM's
 * demo.cpp (https://github.com/goodix-fp-linux-dev/sigfm), with this
 * project's tolerances; that part is also under the MIT licence in
 * LICENSE.SIGFM.
 */

#include "kpmatch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.28318530717958647692

static double sq_distance(const unsigned char *a, const unsigned char *b)
{
  double s = 0.0;
  for (int i = 0; i < KP_DESCRIPTOR; i++) {
    double d = (double)a[i] - (double)b[i];
    s += d * d;
  }
  return s;
}

static long grid(double v)
{
  return lround(v / KP_LOCATION_GRID);
}

static int same_location(const Keypoint *a, const Keypoint *b)
{
  return grid(a->x) == grid(b->x) && grid(a->y) == grid(b->y);
}

static int by_distance(const void *pa, const void *pb)
{
  const KpMatch *a = pa, *b = pb;
  if (a->distance != b->distance) return a->distance < b->distance ? -1 : 1;
  if (a->probe != b->probe) return a->probe < b->probe ? -1 : 1;
  return (a->member > b->member) - (a->member < b->member);
}

size_t kp_match(const Keypoint *probe, size_t probe_count, const Keypoint *member, size_t member_count,
                KpMatch *out)
{
  if (member_count < 2) return 0;
  KpMatch *cand = malloc(sizeof *cand * (probe_count ? probe_count : 1));
  if (!cand) return 0;
  size_t n = 0;
  for (size_t i = 0; i < probe_count; i++) {
    double d1 = INFINITY, d2 = INFINITY;
    int best = -1;
    for (size_t j = 0; j < member_count; j++) {
      double d = sq_distance(probe[i].d, member[j].d);
      if (d < d1) { d2 = d1; d1 = d; best = (int)j; }
      else if (d < d2) d2 = d;
    }
    /* Lowe's ratio test on Euclidean distances: d1 < 0.75 d2, compared squared */
    if (best >= 0 && d1 < KP_RATIO * KP_RATIO * d2) {
      cand[n].probe = (int)i;
      cand[n].member = best;
      cand[n].distance = sqrt(d1);
      n++;
    }
  }
  qsort(cand, n, sizeof *cand, by_distance);
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    int clash = 0;
    for (size_t j = 0; j < kept && !clash; j++)
      clash = same_location(&probe[cand[i].probe], &probe[out[j].probe]) ||
              same_location(&member[cand[i].member], &member[out[j].member]);
    if (!clash) out[kept++] = cand[i];
  }
  free(cand);
  return kept;
}

size_t kp_distinct_locations(const Keypoint *k, size_t count)
{
  size_t distinct = 0;
  for (size_t i = 0; i < count; i++) {
    int seen = 0;
    for (size_t j = 0; j < i && !seen; j++) seen = same_location(&k[i], &k[j]);
    distinct += !seen;
  }
  return distinct;
}

static double circular(double a, double b)
{
  double d = fabs(a - b);
  return d < TWO_PI - d ? d : TWO_PI - d;
}

static int cmp_double(const void *pa, const void *pb)
{
  double a = *(const double *)pa, b = *(const double *)pb;
  return (a > b) - (a < b);
}

static double median(double *v, size_t n)
{
  qsort(v, n, sizeof *v, cmp_double);
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

KpVerdict kp_verify(const Keypoint *probe, const Keypoint *member, const KpMatch *m, size_t count, double cx,
                    double cy)
{
  KpVerdict best;
  memset(&best, 0, sizeof best);
  if (count < 2) return best;
  double *angle = malloc(sizeof *angle * count);
  int *other = malloc(sizeof *other * count);
  if (!angle || !other) { free(angle); free(other); return best; }
  int best_hypothesis_anchor = -1;
  double best_theta = 0.0;
  for (size_t a = 0; a < count; a++) {
    const Keypoint *pa = &probe[m[a].probe], *ma = &member[m[a].member];
    size_t na = 0;
    for (size_t b = 0; b < count; b++) {
      if (b == a) continue;
      const Keypoint *pb = &probe[m[b].probe], *mb = &member[m[b].member];
      double vpx = pb->x - pa->x, vpy = pb->y - pa->y, vmx = mb->x - ma->x, vmy = mb->y - ma->y;
      double lp = hypot(vpx, vpy), lm = hypot(vmx, vmy);
      double shorter = lp < lm ? lp : lm, longer = lp < lm ? lm : lp;
      if (longer <= 0.0 || shorter <= KP_LENGTH_TOLERANCE * longer) continue;
      angle[na] = atan2(vpx * vmy - vpy * vmx, vpx * vmx + vpy * vmy); /* probe vector -> member vector */
      other[na] = (int)b;
      na++;
    }
    for (size_t h = 0; h < na; h++) {
      int support = 1;
      for (size_t j = 0; j < na; j++) support += circular(angle[j], angle[h]) < KP_ANGLE_TOLERANCE;
      if (support > best.score) {
        best.score = support;
        best.anchor = (int)a;
        best_theta = angle[h];
        best_hypothesis_anchor = (int)a;
      }
    }
  }
  if (best_hypothesis_anchor >= 0) {
    /* winning set: the anchor and the matches supporting the winning angle */
    size_t a = (size_t)best_hypothesis_anchor, nw = 0;
    double *wx = malloc(sizeof *wx * count), *wy = malloc(sizeof *wy * count);
    const double c = cos(best_theta), s = sin(best_theta);
    const Keypoint *pa = &probe[m[a].probe], *ma = &member[m[a].member];
    for (size_t b = 0; wx && wy && b < count; b++) {
      int in = (b == a);
      if (!in) {
        const Keypoint *pb = &probe[m[b].probe], *mb = &member[m[b].member];
        double vpx = pb->x - pa->x, vpy = pb->y - pa->y, vmx = mb->x - ma->x, vmy = mb->y - ma->y;
        double lp = hypot(vpx, vpy), lm = hypot(vmx, vmy);
        double shorter = lp < lm ? lp : lm, longer = lp < lm ? lm : lp;
        in = longer > 0.0 && shorter > KP_LENGTH_TOLERANCE * longer &&
             circular(atan2(vpx * vmy - vpy * vmx, vpx * vmx + vpy * vmy), best_theta) < KP_ANGLE_TOLERANCE;
      }
      if (!in) continue;
      const Keypoint *p = &probe[m[b].probe], *q = &member[m[b].member];
      double u = p->x - cx, v = p->y - cy;
      wx[nw] = (q->x - cx) - (c * u - s * v);
      wy[nw] = (q->y - cy) - (s * u + c * v);
      nw++;
    }
    if (wx && wy && nw) {
      best.registered = 1;
      best.theta = best_theta;
      best.dx = median(wx, nw);
      best.dy = median(wy, nw);
    } else {
      best.score = 0;
    }
    free(wx);
    free(wy);
  }
  free(angle);
  free(other);
  return best;
}
