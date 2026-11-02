/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Per-print background reference: built from three untouched (mode-3)
 * acquisitions taken after the last enrolment touch, and checked by two
 * candidate guards before it is stored. Pure arithmetic, no I/O.
 *
 * The guards are heuristics, not established detectors:
 *  - G2, three-frame agreement: the per-pixel spread (max - min) across the
 *    three frames is at most SIDETOUCH_G2_MAX_SPREAD, and the three frame means
 *    lie within SIDETOUCH_G2_MAX_MEAN_RANGE of each other. Catches a finger
 *    arriving, leaving or moving during construction; a finger resting still
 *    passes it.
 *  - G3, level against the touches: the median over the enrolment touches c_i
 *    of mean(r - c_i) is at least SIDETOUCH_G3_MIN_LEVEL. Contact lowers the
 *    frame mean, so a reference taken under a finger sits close to the touches.
 *
 * The bounds were set from offline measurements on one reader; see the
 * constants below.
 */
#pragma once

#include <stdint.h>

#include "sidetouch-format4.h"

#define SIDETOUCH_REFERENCE_FRAMES 3
#define SIDETOUCH_ENROLL_TOUCHES 5

/* About 2.5 and 7 times the maxima measured across three untouched mode-3
 * frames: per-pixel spread at most 18, frame means within 1.1 counts. */
#define SIDETOUCH_G2_MAX_SPREAD 48
#define SIDETOUCH_G2_MAX_MEAN_RANGE 8.0

/* Half the smallest G3 statistic measured over twelve clean references,
 * 57.55; the largest of six finger-contaminated stand-ins was 19.75. */
#define SIDETOUCH_G3_MIN_LEVEL 28.7746055

typedef enum {
  SIDETOUCH_REFERENCE_OK = 0,
  SIDETOUCH_REFERENCE_SPREAD = -1,       /* G2: a pixel disagrees across frames */
  SIDETOUCH_REFERENCE_MEAN_RANGE = -2,   /* G2: the frame means disagree */
  SIDETOUCH_REFERENCE_LEVEL = -3,        /* G3: too close to the touches */
  SIDETOUCH_REFERENCE_INVALID = -4,      /* a NULL argument */
} SidetouchReferenceResult;

/* Diagnostic values, filled as far as each check got; for the driver log. */
typedef struct {
  int    max_spread;
  double mean_range;
  double level;
} SidetouchReferenceStats;

/* Builds the reference as the per-pixel mean of the three frames, computed in
 * 32 bits and rounded to the nearest integer, then applies G2. The reference
 * is written even when G2 fails, so the caller wipes it on every exit. */
SidetouchReferenceResult sidetouch_reference_build (const uint16_t *const     frames[SIDETOUCH_REFERENCE_FRAMES],
                                                    uint16_t                  reference[SIDETOUCH_PIXELS],
                                                    SidetouchReferenceStats  *stats);

/* G3 against the held enrolment touches (decoded samples). */
SidetouchReferenceResult sidetouch_reference_check_level (const uint16_t           reference[SIDETOUCH_PIXELS],
                                                          const uint16_t *const    touches[SIDETOUCH_ENROLL_TOUCHES],
                                                          SidetouchReferenceStats *stats);
