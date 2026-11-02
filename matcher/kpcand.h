/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* The keypoint matcher: preparation, extraction and comparison. */
#ifndef KPCAND_H
#define KPCAND_H

#include "kpmatch.h"

#include <stddef.h>
#include <stdint.h>

enum { KPC_ROWS = 115, KPC_COLS = 113, KPC_PIXELS = KPC_ROWS * KPC_COLS };

/* x = -current (reference NULL) or reference - current; unscorable (-1) if its
 * standard deviation is below 1.0 or its 1st and 99th percentiles coincide;
 * otherwise scaled linearly from the 1st to the 99th percentile onto [0, 1]
 * and clipped (nearest-rank percentiles). */
int kpc_prepare(const int32_t current[KPC_PIXELS], const int32_t *reference, double out[KPC_PIXELS]);

typedef struct {
  Keypoint *k;
  size_t count;
} KpSet;

/* SIFT on a prepared image. Returns 0, or -1 if unscorable (fewer than two
 * distinct described locations) or on allocation failure. */
int kpc_extract(const double image[KPC_PIXELS], KpSet *out);
void kpc_free(KpSet *set);

typedef struct {
  int score;       /* SIGFM support; 0 with no hypothesis */
  int registered;  /* 0: status no-match */
  int matches;     /* matches after the ratio test and one-to-one selection */
  int dx, dy;      /* rounded d in m - c = R_theta (p - c) + d */
  int angle;       /* rounded theta in whole degrees, (-180, 180] */
} KpcResult;

KpcResult kpc_compare(const KpSet *probe, const KpSet *member);

#endif
