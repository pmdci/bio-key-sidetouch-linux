/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-reference on synthetic frames (no biometric data). */
#include "check.h"
#include "sidetouch-reference.h"

static uint16_t f0[SIDETOUCH_PIXELS], f1[SIDETOUCH_PIXELS], f2[SIDETOUCH_PIXELS];
static uint16_t ref[SIDETOUCH_PIXELS];
static uint16_t t[SIDETOUCH_ENROLL_TOUCHES][SIDETOUCH_PIXELS];

static void
fill (uint16_t *s, int v)
{
  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    s[k] = (uint16_t) v;
}

static SidetouchReferenceResult
build (SidetouchReferenceStats *st)
{
  const uint16_t *const frames[SIDETOUCH_REFERENCE_FRAMES] = { f0, f1, f2 };

  return sidetouch_reference_build (frames, ref, st);
}

static SidetouchReferenceResult
level (SidetouchReferenceStats *st)
{
  const uint16_t *const touches[SIDETOUCH_ENROLL_TOUCHES] = { t[0], t[1], t[2], t[3], t[4] };

  return sidetouch_reference_check_level (ref, touches, st);
}

/* Touch i sits d[i] counts below the reference everywhere; touch `extra_at`
 * additionally sits one count lower on its first `extra` pixels. */
static void
touches (const int d[SIDETOUCH_ENROLL_TOUCHES], int extra_at, int extra)
{
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    for (int k = 0; k < SIDETOUCH_PIXELS; k++)
      t[i][k] = (uint16_t) (ref[k] - d[i] - (i == extra_at && k < extra));
}

int
main (void)
{
  SidetouchReferenceStats st;

  /* rounding to nearest: sums 0..5 over three frames */
  fill (f0, 1000);
  fill (f1, 1000);
  fill (f2, 1000);
  f2[0] = 1001;                    /* 3001 / 3 = 1000.33 -> 1000 */
  f1[1] = 1001, f2[1] = 1001;      /* 3002 / 3 = 1000.67 -> 1001 */
  f0[2] = f1[2] = f2[2] = 1001;    /* exact */
  CHECK (build (&st) == SIDETOUCH_REFERENCE_OK);
  CHECK (ref[0] == 1000 && ref[1] == 1001 && ref[2] == 1001 && ref[5] == 1000);
  CHECK (st.max_spread == 1);

  /* pixel values at the ends of the range, all frames equal */
  f0[3] = f1[3] = f2[3] = 65535;
  f0[4] = f1[4] = f2[4] = 0;
  CHECK (build (&st) == SIDETOUCH_REFERENCE_OK);
  CHECK (ref[3] == 65535 && ref[4] == 0 && st.max_spread == 1);
  f0[3] = f1[3] = f2[3] = 1000;
  f0[4] = f1[4] = f2[4] = 1000;

  /* G2 spread: 48 passes, 49 fails, whichever frame carries it */
  f0[100] = 1000 + 48;
  CHECK (build (&st) == SIDETOUCH_REFERENCE_OK && st.max_spread == 48);
  f0[100] = 1000;
  f2[200] = 1000 - 49;
  CHECK (build (&st) == SIDETOUCH_REFERENCE_SPREAD && st.max_spread == 49);
  f2[200] = 1000;

  /* G2 mean range: a whole frame shifted by 8 passes, by 9 fails */
  fill (f0, 1000);
  fill (f1, 1008);
  fill (f2, 1004);
  CHECK (build (&st) == SIDETOUCH_REFERENCE_OK && st.mean_range == 8.0 && st.max_spread == 8);
  fill (f1, 1009);
  CHECK (build (&st) == SIDETOUCH_REFERENCE_MEAN_RANGE && st.mean_range == 9.0);
  CHECK (ref[0] == 1004);   /* (1000 + 1009 + 1004) / 3 = 1004.33 */

  /* G3: median of the five mean(r - c_i), at the boundary of L = 28.7746055.
   * 28 + n / 12995 >= L first holds at n = 10066. */
  fill (f0, 1000);
  fill (f1, 1000);
  fill (f2, 1000);
  CHECK (build (&st) == SIDETOUCH_REFERENCE_OK);
  {
    const int d[SIDETOUCH_ENROLL_TOUCHES] = { 500, -100, 28, 600, 0 };

    touches (d, 2, 10066);
    CHECK (level (&st) == SIDETOUCH_REFERENCE_OK);
    CHECK (st.level > SIDETOUCH_G3_MIN_LEVEL && st.level < 28.77461);
    touches (d, 2, 10065);
    CHECK (level (&st) == SIDETOUCH_REFERENCE_LEVEL);
    CHECK (st.level < SIDETOUCH_G3_MIN_LEVEL && st.level > 28.77452);
  }
  {
    /* the median, not the mean or the largest value, decides */
    const int d[SIDETOUCH_ENROLL_TOUCHES] = { 900, 900, 10, 10, 10 };

    touches (d, -1, 0);
    CHECK (level (&st) == SIDETOUCH_REFERENCE_LEVEL && st.level == 10.0);
  }
  {
    /* a reference below the touches (negative level) fails */
    const int d[SIDETOUCH_ENROLL_TOUCHES] = { -40, -40, -40, -40, -40 };

    touches (d, -1, 0);
    CHECK (level (&st) == SIDETOUCH_REFERENCE_LEVEL && st.level == -40.0);
  }

  /* invalid arguments */
  {
    const uint16_t *const frames[SIDETOUCH_REFERENCE_FRAMES] = { f0, NULL, f2 };
    const uint16_t *const held[SIDETOUCH_ENROLL_TOUCHES] = { t[0], t[1], NULL, t[3], t[4] };

    CHECK (sidetouch_reference_build (frames, ref, &st) == SIDETOUCH_REFERENCE_INVALID);
    CHECK (sidetouch_reference_build (NULL, ref, &st) == SIDETOUCH_REFERENCE_INVALID);
    CHECK (sidetouch_reference_check_level (ref, held, &st) == SIDETOUCH_REFERENCE_INVALID);
    CHECK (sidetouch_reference_check_level (NULL, held, &st) == SIDETOUCH_REFERENCE_INVALID);
  }

  CHECK_DONE ("test-sidetouch-reference");
}
