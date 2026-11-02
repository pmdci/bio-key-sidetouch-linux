/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for kpmatch.c: per-anchor verification, registration convention,
 * ratio test and one-to-one locations, on constructed keypoints. */
#include "kpmatch.h"

#include <math.h>
#include <stdio.h>
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
static const double CX = 56.0, CY = 57.0;

/* member = R_theta (probe - c) + c + d */
static void place(Keypoint *p, Keypoint *m, double px, double py, double theta, double dx, double dy)
{
  memset(p, 0, sizeof *p);
  memset(m, 0, sizeof *m);
  p->x = px; p->y = py;
  double u = px - CX, v = py - CY;
  m->x = cos(theta) * u - sin(theta) * v + CX + dx;
  m->y = sin(theta) * u + cos(theta) * v + CY + dy;
}

static void identity_matches(KpMatch *mt, size_t n)
{
  for (size_t i = 0; i < n; i++) { mt[i].probe = (int)i; mt[i].member = (int)i; mt[i].distance = (double)i; }
}

static void test_four_rigid_matches_score_four(void)
{
  Keypoint p[4], m[4];
  double pts[4][2] = {{40, 40}, {70, 45}, {50, 75}, {65, 70}};
  for (int i = 0; i < 4; i++) place(&p[i], &m[i], pts[i][0], pts[i][1], 0.0, 3.0, -2.0);
  KpMatch mt[4];
  identity_matches(mt, 4);
  KpVerdict v = kp_verify(p, m, mt, 4, CX, CY);
  CHECK(v.score == 4 && v.registered, "four rigid matches score %d", v.score);
  CHECK(fabs(v.theta) < 1e-9 && fabs(v.dx - 3.0) < 1e-9 && fabs(v.dy + 2.0) < 1e-9,
        "registration theta %g d (%g, %g)", v.theta, v.dx, v.dy);
}

static void test_rotation_45_registration(void)
{
  Keypoint p[5], m[5];
  double pts[5][2] = {{40, 40}, {70, 45}, {50, 75}, {65, 70}, {56, 50}};
  double theta = 45.0 * PI / 180.0;
  for (int i = 0; i < 5; i++) place(&p[i], &m[i], pts[i][0], pts[i][1], theta, -6.0, 4.0);
  KpMatch mt[5];
  identity_matches(mt, 5);
  KpVerdict v = kp_verify(p, m, mt, 5, CX, CY);
  CHECK(v.score == 5, "45-degree rigid set scores %d", v.score);
  CHECK(fabs(v.theta - theta) < 1e-9 && fabs(v.dx + 6.0) < 1e-9 && fabs(v.dy - 4.0) < 1e-9,
        "45-degree registration theta %g d (%g, %g)", v.theta * 180.0 / PI, v.dx, v.dy);
}

static void test_disconnected_groups_do_not_pool(void)
{
  /* two rigid groups of three whose relative displacement no near-rigid map
   * explains: group 2 moves 40 px further along the line between the groups,
   * so every cross-group pair fails the length test. (A far group whose offset
   * looks like a small rotation plus <10 % scale from an anchor is, by design,
   * consistent within the deformation tolerances.) */
  Keypoint p[6], m[6];
  double pts[6][2] = {{30, 30}, {45, 32}, {35, 48}, {80, 80}, {95, 82}, {85, 98}};
  for (int i = 0; i < 3; i++) place(&p[i], &m[i], pts[i][0], pts[i][1], 0.0, 0.0, 0.0);
  for (int i = 3; i < 6; i++) place(&p[i], &m[i], pts[i][0], pts[i][1], 0.0, 40.0, 40.0);
  KpMatch mt[6];
  identity_matches(mt, 6);
  KpVerdict v = kp_verify(p, m, mt, 6, CX, CY);
  CHECK(v.score == 3, "disconnected groups pooled: score %d, expected 3", v.score);
}

static void test_fewer_than_two_matches_no_registration(void)
{
  Keypoint p[1], m[1];
  place(&p[0], &m[0], 50, 50, 0.0, 0.0, 0.0);
  KpMatch mt[1];
  identity_matches(mt, 1);
  KpVerdict v = kp_verify(p, m, mt, 1, CX, CY);
  CHECK(v.score == 0 && !v.registered, "single match: score %d registered %d", v.score, v.registered);
  v = kp_verify(p, m, mt, 0, CX, CY);
  CHECK(v.score == 0 && !v.registered, "no matches");
}

static void test_tolerances(void)
{
  /* anchor at (40,40); b at 20 px; member b moved to change length / angle */
  Keypoint p[2], m[2];
  KpMatch mt[2];
  identity_matches(mt, 2);
  place(&p[0], &m[0], 40, 40, 0.0, 0.0, 0.0);
  place(&p[1], &m[1], 60, 40, 0.0, 0.0, 0.0);
  m[1].x = 40 + 20 * 0.92; /* length ratio 0.92 > 0.90: kept */
  CHECK(kp_verify(p, m, mt, 2, CX, CY).score == 2, "length ratio 0.92 accepted");
  m[1].x = 40 + 20 * 0.88; /* 0.88: rejected */
  CHECK(kp_verify(p, m, mt, 2, CX, CY).score == 0, "length ratio 0.88 rejected");
  /* angle tolerance is relative between supporting matches: three matches whose
   * pair angles differ by 0.08 rad cluster; by 0.12 rad they do not */
  Keypoint q[3], n[3];
  KpMatch mq[3];
  identity_matches(mq, 3);
  place(&q[0], &n[0], 40, 40, 0.0, 0.0, 0.0);
  place(&q[1], &n[1], 60, 40, 0.0, 0.0, 0.0);
  place(&q[2], &n[2], 40, 60, 0.0, 0.0, 0.0);
  double a = 0.08; /* rotate member's third point about the anchor by a */
  n[2].x = 40 - 20 * sin(a); n[2].y = 40 + 20 * cos(a);
  CHECK(kp_verify(q, n, mq, 3, CX, CY).score == 3, "angle spread 0.08 rad clusters");
  a = 0.40; /* checked by hand: no anchor sees both other matches within 0.10 rad */
  n[2].x = 40 - 20 * sin(a); n[2].y = 40 + 20 * cos(a);
  CHECK(kp_verify(q, n, mq, 3, CX, CY).score == 2, "angle deviation 0.40 rad does not cluster");
}

static void fill(Keypoint *k, double x, double y, unsigned char base)
{
  memset(k, 0, sizeof *k);
  k->x = x; k->y = y;
  for (int i = 0; i < KP_DESCRIPTOR; i++) k->d[i] = (unsigned char)((base * 7 + i * 13) & 0xff);
}

static void test_ratio_and_one_to_one(void)
{
  Keypoint probe[3], member[4];
  fill(&probe[0], 10, 10, 1);
  fill(&probe[1], 30, 30, 2);
  probe[2] = probe[1]; probe[2].theta = 1.0; /* second orientation at the same location */
  fill(&member[0], 11, 10, 1);
  fill(&member[1], 31, 30, 2);
  fill(&member[2], 50, 50, 9);
  fill(&member[3], 70, 70, 13);
  KpMatch out[3];
  size_t n = kp_match(probe, 3, member, 4, out);
  CHECK(n == 2, "distinct locations count once: %zu matches", n);
  CHECK(n >= 1 && out[0].distance <= out[n - 1].distance, "ascending distance order");
  /* ambiguous: two member descriptors identical -> ratio 1, rejected */
  Keypoint amb[2];
  fill(&amb[0], 10, 10, 1);
  fill(&amb[1], 40, 40, 1);
  n = kp_match(probe, 1, amb, 2, out);
  CHECK(n == 0, "ambiguous nearest neighbours rejected by the ratio test: %zu", n);
  CHECK(kp_distinct_locations(probe, 3) == 2, "distinct locations %zu", kp_distinct_locations(probe, 3));
}

int main(void)
{
  test_four_rigid_matches_score_four();
  test_rotation_45_registration();
  test_disconnected_groups_do_not_pool();
  test_fewer_than_two_matches_no_registration();
  test_tolerances();
  test_ratio_and_one_to_one();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
