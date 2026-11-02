/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* SIFT keypoints and descriptors (Lowe, IJCV 60(2), 2004), implemented to the
 * specification and default parameters of Rey-Otero and Delbracio, "Anatomy of
 * the SIFT Method", IPOL 4 (2014), DOI 10.5201/ipol.2014.82, Algorithms 1-12
 * and Tables 3-4. One deviation, from the keypoint candidate specification:
 * the descriptor border check requires the oriented descriptor patch to lie
 * inside the image instead of IPOL's orientation-free sqrt(2)*lambda*sigma
 * margin. Pure C11, deterministic. */
#ifndef SIFT_H
#define SIFT_H

#include "kpmatch.h"

#include <stddef.h>

#define SIFT_SIGMA_MIN 0.8
#define SIFT_DELTA_MIN 0.5
#define SIFT_SIGMA_IN 0.5
#define SIFT_N_SPO 3
#define SIFT_C_DOG 0.015
#define SIFT_C_EDGE 10.0
#define SIFT_N_BINS 36
#define SIFT_LAMBDA_ORI 1.5
#define SIFT_T 0.80
#define SIFT_N_HIST 4
#define SIFT_N_ORI 8
#define SIFT_LAMBDA_DESCR 6.0

/* Detect and describe keypoints in a rows x cols image (row-major, values in
 * [0, 1]). Returns the number of keypoints and sets *out (malloc'd; caller
 * frees), or -1 on allocation failure. */
int sift_detect(const double *image, int rows, int cols, Keypoint **out);

/* Number of octaves for an image: floor(log2(min(rows, cols) / delta_min / 12) + 1). */
int sift_octaves(int rows, int cols);

#endif
