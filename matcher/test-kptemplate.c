/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for kptemplate.c on synthetic samples (no biometric data). */
#include "kptemplate.h"

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

static int extract(unsigned seed, int ox, int oy, unsigned noise, KpSet *out)
{
  static int32_t raw[KPC_PIXELS], ref[KPC_PIXELS];
  static double img[KPC_PIXELS];
  finger(seed, ox, oy, noise, raw);
  for (int i = 0; i < KPC_PIXELS; i++) ref[i] = 20000;
  return kpc_prepare(raw, ref, img) == 0 ? kpc_extract(img, out) : -1;
}

static KpSet sets[5];
static KpTemplate tpl;

static void test_enrol_store_reload_verify(void)
{
  int shifts[5][2] = {{20, 20}, {22, 21}, {18, 23}, {24, 18}, {20, 26}};
  const KpSet *in[5];
  for (int i = 0; i < 5; i++) {
    CHECK(extract(101u, shifts[i][0], shifts[i][1], 10u + (unsigned)i, &sets[i]) == 0, "member %d", i);
    in[i] = &sets[i];
  }
  CHECK(kpt_enrol(in, 5, &tpl) == 0 && tpl.members == 5, "enrol five members");
  size_t n = kpt_serialised_size(&tpl);
  unsigned char *buf = malloc(n), *again = malloc(n);
  CHECK(kpt_serialise(&tpl, buf, n) == n, "serialise %zu bytes", n);
  KpTemplate back;
  CHECK(kpt_deserialise(buf, n, &back) == 0 && back.members == 5, "reload");
  CHECK(kpt_serialise(&back, again, n) == n && memcmp(buf, again, n) == 0, "re-serialisation is byte-identical");
  KpSet probe, other;
  CHECK(extract(101u, 23, 19, 77u, &probe) == 0 && extract(202u, 20, 20, 78u, &other) == 0, "probes");
  KptDecision g0 = kpt_verify(&tpl, &probe), g1 = kpt_verify(&back, &probe);
  KptDecision i1 = kpt_verify(&back, &other);
  CHECK(g0.score == g1.score && g0.accept == g1.accept, "same decision before and after storage: %d vs %d", g0.score,
        g1.score);
  CHECK(g1.accept && g1.score > KPT_THRESHOLD, "genuine accepted: %d", g1.score);
  CHECK(!i1.accept, "impostor rejected: %d", i1.score);
  kpt_free(&back);
  /* malformed inputs (a refused reload frees its own partial allocations) */
  CHECK(kpt_deserialise(buf, n - 1, &back) == -1, "truncated refused");
  unsigned char *bad = malloc(n);
  memcpy(bad, buf, n); bad[0] = 'X';
  CHECK(kpt_deserialise(bad, n, &back) == -1, "bad magic refused");
  memcpy(bad, buf, n); bad[4] = 2;
  CHECK(kpt_deserialise(bad, n, &back) == -1, "wrong version refused");
  memcpy(bad, buf, n); bad[6] = 9;
  CHECK(kpt_deserialise(bad, n, &back) == -1, "member count out of range refused");
  memcpy(bad, buf, n); bad[7] = 0xff; bad[8] = 0xff;
  CHECK(kpt_deserialise(bad, n, &back) == -1, "keypoint count beyond the data refused");
  unsigned char *longer = malloc(n + 1);
  memcpy(longer, buf, n); longer[n] = 0;
  CHECK(kpt_deserialise(longer, n + 1, &back) == -1, "trailing bytes refused");
  free(buf); free(again); free(bad); free(longer);
  kpt_free(&back);
  kpc_free(&probe); kpc_free(&other);
}

static void test_enrol_needs_three_members(void)
{
  const KpSet *in[5] = {&sets[0], &sets[1], NULL, NULL, NULL};
  KpTemplate t;
  CHECK(kpt_enrol(in, 5, &t) == -1, "two valid members fail enrolment");
  const KpSet *three[5] = {&sets[0], NULL, &sets[2], NULL, &sets[4]};
  CHECK(kpt_enrol(three, 5, &t) == 0 && t.members == 3, "three valid members enrol");
  kpt_free(&t);
}

/* the frozen candidate has no keypoint cap: only the u16 count field bounds a member */
static void test_large_member_kept(void)
{
  KpSet big = {malloc(sizeof(Keypoint) * 5000), 5000};
  for (size_t i = 0; i < big.count; i++) big.k[i] = sets[0].k[i % sets[0].count];
  const KpSet *in[3] = {&big, &sets[1], &sets[2]};
  KpTemplate t;
  CHECK(kpt_enrol(in, 3, &t) == 0 && t.members == 3 && t.member[0].count == 5000, "5000-keypoint member kept");
  size_t n = kpt_serialised_size(&t);
  unsigned char *buf = malloc(n);
  KpTemplate back;
  CHECK(kpt_serialise(&t, buf, n) == n && kpt_deserialise(buf, n, &back) == 0 && back.member[0].count == 5000,
        "5000-keypoint member stored and reloaded");
  free(buf);
  kpt_free(&back);
  kpt_free(&t);
  kpc_free(&big);
}

int main(void)
{
  test_enrol_store_reload_verify();
  test_enrol_needs_three_members();
  test_large_member_kept();
  kpt_free(&tpl);
  for (int i = 0; i < 5; i++) kpc_free(&sets[i]);
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
