/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
#include "kpcand.h"
#include "sift.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *pa, const void *pb)
{
  double a = *(const double *)pa, b = *(const double *)pb;
  return (a > b) - (a < b);
}

int kpc_prepare(const int32_t current[KPC_PIXELS], const int32_t *reference, double out[KPC_PIXELS])
{
  double sum = 0.0, squares = 0.0;
  for (int i = 0; i < KPC_PIXELS; i++) {
    out[i] = (reference ? (double)reference[i] : 0.0) - (double)current[i];
    sum += out[i];
  }
  double mean = sum / (double)KPC_PIXELS;
  for (int i = 0; i < KPC_PIXELS; i++) squares += (out[i] - mean) * (out[i] - mean);
  if (sqrt(squares / (double)KPC_PIXELS) < 1.0) return -1;
  double *sorted = malloc(sizeof *sorted * KPC_PIXELS);
  if (!sorted) return -1;
  memcpy(sorted, out, sizeof *sorted * KPC_PIXELS);
  qsort(sorted, KPC_PIXELS, sizeof *sorted, cmp_double);
  /* nearest-rank 1st and 99th percentiles */
  double lo = sorted[(size_t)lround(0.01 * (KPC_PIXELS - 1))], hi = sorted[(size_t)lround(0.99 * (KPC_PIXELS - 1))];
  free(sorted);
  if (!(hi > lo)) return -1;
  for (int i = 0; i < KPC_PIXELS; i++) {
    double v = (out[i] - lo) / (hi - lo);
    out[i] = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
  }
  return 0;
}

int kpc_extract(const double image[KPC_PIXELS], KpSet *out)
{
  out->k = NULL;
  out->count = 0;
  Keypoint *k = NULL;
  int n = sift_detect(image, KPC_ROWS, KPC_COLS, &k);
  if (n < 0) return -1;
  out->k = k;
  out->count = (size_t)n;
  return kp_distinct_locations(k, (size_t)n) >= 2 ? 0 : -1;
}

void kpc_free(KpSet *set)
{
  free(set->k);
  set->k = NULL;
  set->count = 0;
}

KpcResult kpc_compare(const KpSet *probe, const KpSet *member)
{
  KpcResult r;
  memset(&r, 0, sizeof r);
  if (probe->count == 0 || member->count < 2) return r;
  KpMatch *m = malloc(sizeof *m * probe->count);
  if (!m) return r;
  size_t n = kp_match(probe->k, probe->count, member->k, member->count, m);
  KpVerdict v = kp_verify(probe->k, member->k, m, n, (KPC_COLS - 1) / 2.0, (KPC_ROWS - 1) / 2.0);
  free(m);
  r.matches = (int)n;
  r.score = v.score;
  r.registered = v.registered;
  if (v.registered) {
    long deg = lround(v.theta * 180.0 / 3.14159265358979323846);
    r.angle = (int)(deg == -180 ? 180 : deg);
    r.dx = (int)lround(v.dx);
    r.dy = (int)lround(v.dy);
  }
  return r;
}
