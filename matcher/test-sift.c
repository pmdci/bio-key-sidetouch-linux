/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sift.c on synthetic images (no biometric data), including
 * end-to-end matching with kpmatch.c. */
#include "kpmatch.h"
#include "sift.h"

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

#define ROWS 115
#define COLS 113
static const double PI = 3.14159265358979323846;

static double uniform(unsigned *s)
{
  *s = *s * 1103515245u + 12345u;
  return (double)((*s >> 8) & 0xffffu) / 65535.0;
}

/* plane-wave "finger" texture sampled at (x + ox, y + oy), scaled to [0, 1] */
static void texture(unsigned seed, double ox, double oy, unsigned noise_seed, double *img)
{
  double f[24], a[24], p[24];
  for (int i = 0; i < 24; i++) { f[i] = 0.11 + 0.05 * uniform(&seed); a[i] = PI * uniform(&seed); p[i] = 2 * PI * uniform(&seed); }
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      double v = 0;
      for (int i = 0; i < 24; i++) v += cos(2 * PI * f[i] * ((c + ox) * cos(a[i]) + (r + oy) * sin(a[i])) + p[i]);
      v = v / sqrt(24.0) + 0.05 * (uniform(&noise_seed) - 0.5);
      img[r * COLS + c] = 0.5 + 0.18 * v;
    }
  for (int i = 0; i < ROWS * COLS; i++) img[i] = img[i] < 0 ? 0 : img[i] > 1 ? 1 : img[i];
}

static void test_octaves(void)
{
  CHECK(sift_octaves(ROWS, COLS) == 5, "octaves for 115 x 113: %d", sift_octaves(ROWS, COLS));
}

static void test_flat_image_has_no_keypoints(void)
{
  static double img[ROWS * COLS];
  for (int i = 0; i < ROWS * COLS; i++) img[i] = 0.5;
  Keypoint *k = NULL;
  int n = sift_detect(img, ROWS, COLS, &k);
  CHECK(n == 0, "flat image: %d keypoints", n);
  free(k);
}

static void test_blob_detected_at_centre(void)
{
  static double img[ROWS * COLS];
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      img[r * COLS + c] = 0.2 + 0.6 * exp(-((r - 57.0) * (r - 57.0) + (c - 56.0) * (c - 56.0)) / (2 * 4.0 * 4.0));
  Keypoint *k = NULL;
  int n = sift_detect(img, ROWS, COLS, &k);
  int found = 0;
  for (int i = 0; i < n; i++)
    if (hypot(k[i].x - 56.0, k[i].y - 57.0) < 1.5 && k[i].sigma > 2.0 && k[i].sigma < 8.0) found = 1;
  CHECK(found, "blob (std 4) not found at the centre among %d keypoints", n);
  free(k);
}

static void test_deterministic_and_patches_inside(void)
{
  static double img[ROWS * COLS];
  texture(101u, 20, 20, 7u, img);
  Keypoint *a = NULL, *b = NULL;
  int na = sift_detect(img, ROWS, COLS, &a), nb = sift_detect(img, ROWS, COLS, &b);
  CHECK(na > 10, "texture yields %d keypoints", na);
  CHECK(na == nb && memcmp(a, b, sizeof *a * (size_t)na) == 0, "deterministic");
  double half_factor = SIFT_LAMBDA_DESCR * (SIFT_N_HIST + 1) / SIFT_N_HIST;
  int outside = 0;
  for (int i = 0; i < na; i++) {
    double h = half_factor * a[i].sigma, c = cos(a[i].theta), s = sin(a[i].theta);
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) {
        /* corner in (row, col) as the descriptor's normalised frame maps back */
        double dr = h * (sx * c - sy * s), dc = h * (sx * s + sy * c);
        double row = a[i].y + dr, col = a[i].x + dc;
        if (row < 0 || col < 0 || row > ROWS - 1 || col > COLS - 1) outside++;
      }
  }
  CHECK(outside == 0, "%d descriptor patch corners outside the image", outside);
  free(a); free(b);
}

static KpVerdict compare(const double *probe, const double *member, int *matches)
{
  Keypoint *p = NULL, *m = NULL;
  int np = sift_detect(probe, ROWS, COLS, &p), nm = sift_detect(member, ROWS, COLS, &m);
  KpMatch *mt = malloc(sizeof *mt * (size_t)(np > 0 ? np : 1));
  size_t n = kp_match(p, (size_t)np, m, (size_t)nm, mt);
  KpVerdict v = kp_verify(p, m, mt, n, (COLS - 1) / 2.0, (ROWS - 1) / 2.0);
  if (matches) *matches = (int)n;
  free(p); free(m); free(mt);
  return v;
}

static void test_rotation_90_recovered(void)
{
  /* square crop rotated exactly by 90 degrees (no interpolation) */
  static double img[ROWS * COLS], rot[ROWS * COLS];
  texture(101u, 20, 20, 9u, img);
  for (int i = 0; i < ROWS * COLS; i++) rot[i] = 0.5;
  /* rotate the central 113 x 113 square: rot(r, c) = img(c', r') with a 90-degree map */
  int off = (ROWS - COLS) / 2;
  for (int r = 0; r < COLS; r++)
    for (int c = 0; c < COLS; c++) rot[(r + off) * COLS + c] = img[(COLS - 1 - c + off) * COLS + r];
  KpVerdict v = compare(rot, img, NULL);
  CHECK(v.registered && v.score >= 5, "90-degree rotation: score %d", v.score);
  CHECK(fabs(fabs(v.theta) - PI / 2) < 0.1, "90-degree rotation angle %g degrees", v.theta * 180 / PI);
}

static void test_translation_and_impostor(void)
{
  static double member[ROWS * COLS], probe[ROWS * COLS], other[ROWS * COLS];
  texture(101u, 20, 20, 11u, member);
  texture(101u, 25, 17, 12u, probe);   /* m = p + (5, -3) */
  texture(202u, 20, 20, 13u, other);
  KpVerdict g = compare(probe, member, NULL);
  KpVerdict i = compare(other, member, NULL);
  CHECK(g.registered && fabs(g.theta) < 0.1 && fabs(g.dx - 5.0) <= 1.0 && fabs(g.dy + 3.0) <= 1.0,
        "translation: theta %g d (%g, %g) score %d", g.theta, g.dx, g.dy, g.score);
  CHECK(g.score >= 5 && g.score > 2 * i.score, "genuine %d vs impostor %d", g.score, i.score);
}

int main(void)
{
  test_octaves();
  test_flat_image_has_no_keypoints();
  test_blob_detected_at_centre();
  test_deterministic_and_patches_inside();
  test_rotation_90_recovered();
  test_translation_and_impostor();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
