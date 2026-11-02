/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Keypoint matching and geometric verification for the keypoint matcher:
 * Lowe's ratio test at SIGFM's 0.75, one-to-one matching on
 * distinct locations, and SIGFM's per-anchor geometric verification
 * (github.com/goodix-fp-linux-dev/sigfm, demo.cpp) with this project's
 * tolerances. Pure C11, deterministic. Positions are (x = column, y = row) in
 * input pixels. */
#ifndef KPMATCH_H
#define KPMATCH_H

#include <stddef.h>

enum { KP_DESCRIPTOR = 128 };

typedef struct {
  double x, y;                       /* column, row in input pixels */
  double sigma, theta;
  unsigned char d[KP_DESCRIPTOR];
} Keypoint;

typedef struct {
  int probe, member;                 /* keypoint indices */
  double distance;                   /* Euclidean descriptor distance */
} KpMatch;

#define KP_RATIO 0.75
#define KP_LENGTH_TOLERANCE 0.90
#define KP_ANGLE_TOLERANCE 0.10      /* radians */
#define KP_LOCATION_GRID 0.5         /* pixels */

/* Ratio-test matches made one-to-one on distinct locations, in ascending
 * distance order (ties: probe index, then member index). Returns the count
 * written to out (capacity probe_count). */
size_t kp_match(const Keypoint *probe, size_t probe_count, const Keypoint *member, size_t member_count,
                KpMatch *out);

/* Number of distinct keypoint locations (positions on the 0.5-px grid). */
size_t kp_distinct_locations(const Keypoint *k, size_t count);

typedef struct {
  int score;                         /* largest support; 0 if no hypothesis */
  int registered;                    /* 1 if a winning hypothesis exists */
  double theta;                      /* radians: probe vectors rotated by theta give member vectors */
  double dx, dy;                     /* m - c = R_theta (p - c) + d, median over the winning set */
  int anchor;                        /* index into the matches of the winning anchor */
} KpVerdict;

/* SIGFM per-anchor verification over the given matches. cx, cy: image centre. */
KpVerdict kp_verify(const Keypoint *probe, const Keypoint *member, const KpMatch *matches, size_t count,
                    double cx, double cy);

#endif
