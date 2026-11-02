/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-match on synthetic textures (no biometric data): the
 * driver path must give exactly what the frozen candidate gives when called
 * directly, and handle unscorable touches. */
#include <math.h>
#include <stdint.h>

#include "check.h"
#include "sidetouch-match.h"

#define BLOBS 60

static double bx[BLOBS], by[BLOBS], bs[BLOBS], ba[BLOBS];

static void
blobs (unsigned seed)
{
  unsigned x = seed * 2654435761u + 7u;

  for (int i = 0; i < BLOBS; i++)
    {
      x = x * 1103515245u + 12345u;
      bx[i] = (double) ((x >> 8) % 1300) / 10.0 - 10.0;
      x = x * 1103515245u + 12345u;
      by[i] = (double) ((x >> 8) % 1350) / 10.0 - 10.0;
      x = x * 1103515245u + 12345u;
      bs[i] = 1.5 + (double) ((x >> 8) % 30) / 10.0;
      x = x * 1103515245u + 12345u;
      ba[i] = (double) ((x >> 8) % 2 ? 1 : -1) * (80.0 + (double) ((x >> 12) % 120));
    }
}

/* reference minus a texture of Gaussian blobs, shifted by (dx, dy) */
static void
touch (uint16_t *out, const uint16_t *reference, double dx, double dy)
{
  for (int r = 0; r < SIDETOUCH_ROWS; r++)
    for (int c = 0; c < SIDETOUCH_COLS; c++)
      {
        double v = 0.0;

        for (int i = 0; i < BLOBS; i++)
          {
            double ex = c - dx - bx[i], ey = r - dy - by[i];

            v += ba[i] * exp (-(ex * ex + ey * ey) / (2.0 * bs[i] * bs[i]));
          }
        v = reference[r * SIDETOUCH_COLS + c] - 300.0 - v;
        out[r * SIDETOUCH_COLS + c] = (uint16_t) lround (v < 0 ? 0 : v > 65535 ? 65535 : v);
      }
}

/* the frozen candidate called directly, as the milestone-1 scoring tool does */
static int
direct (const uint16_t *samples, const uint16_t *reference, KpSet *out)
{
  static int32_t cur[KPC_PIXELS], ref[KPC_PIXELS];
  static double prep[KPC_PIXELS];

  for (int k = 0; k < KPC_PIXELS; k++)
    {
      cur[k] = samples[k];
      ref[k] = reference[k];
    }
  out->k = NULL;
  out->count = 0;
  if (kpc_prepare (cur, ref, prep) != 0 || kpc_extract (prep, out) != 0)
    {
      kpc_free (out);
      return -1;
    }
  return 0;
}

static int
same_set (const KpSet *a, const KpSet *b)
{
  return a->count == b->count && (a->count == 0 || memcmp (a->k, b->k, a->count * sizeof *a->k) == 0);
}

static uint16_t ref[SIDETOUCH_PIXELS], flat[SIDETOUCH_PIXELS], single[SIDETOUCH_PIXELS];
static uint16_t t[SIDETOUCH_ENROLL_TOUCHES][SIDETOUCH_PIXELS], probe[SIDETOUCH_PIXELS], other[SIDETOUCH_PIXELS];

int
main (void)
{
  KpSet a, b;
  KpTemplate tpl, dtpl;
  KptDecision d, dd;
  const uint16_t *touches[SIDETOUCH_ENROLL_TOUCHES] = { t[0], t[1], t[2], t[3], t[4] };
  const double shift[SIDETOUCH_ENROLL_TOUCHES][2] = { { 0, 0 }, { 2, -1 }, { -3, 2 }, { 1, 3 }, { -2, -2 } };
  int dropped = -1;

  /* a reference with structure of its own, so subtraction matters */
  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    ref[k] = (uint16_t) (1200 + (k % SIDETOUCH_COLS) * 2 + ((k / SIDETOUCH_COLS) % 7) * 5);
  memcpy (flat, ref, sizeof flat);

  blobs (1);
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    touch (t[i], ref, shift[i][0], shift[i][1]);
  touch (probe, ref, 1.0, -2.0);
  blobs (2);
  touch (other, ref, 0.0, 0.0);

  /* features: identical to the direct call */
  CHECK (sidetouch_match_features (t[0], ref, &a) == SIDETOUCH_MATCH_OK);
  CHECK (direct (t[0], ref, &b) == 0);
  CHECK (a.count >= 2 && same_set (&a, &b));
  kpc_free (&a);
  kpc_free (&b);

  /* enrolment and verification: identical to the direct path */
  CHECK (sidetouch_match_enrol (touches, ref, &tpl, &dropped) == SIDETOUCH_MATCH_OK);
  CHECK (dropped == 0 && tpl.members == SIDETOUCH_ENROLL_TOUCHES);
  {
    KpSet sets[SIDETOUCH_ENROLL_TOUCHES];
    const KpSet *in[SIDETOUCH_ENROLL_TOUCHES];

    for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
      {
        CHECK (direct (t[i], ref, &sets[i]) == 0);
        in[i] = &sets[i];
      }
    CHECK (kpt_enrol (in, SIDETOUCH_ENROLL_TOUCHES, &dtpl) == 0);
    for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
      {
        CHECK (same_set (&tpl.member[i], &dtpl.member[i]));
        kpc_free (&sets[i]);
      }
  }
  CHECK (sidetouch_match_verify (&tpl, ref, probe, &d) == SIDETOUCH_MATCH_OK);
  CHECK (direct (probe, ref, &b) == 0);
  dd = kpt_verify (&dtpl, &b);
  kpc_free (&b);
  CHECK (d.score == dd.score && d.accept == dd.accept);
  CHECK (d.accept && d.score >= 7);   /* the synthetic genuine probe is accepted */
  printf ("genuine synthetic score %d\n", d.score);

  CHECK (sidetouch_match_verify (&tpl, ref, other, &d) == SIDETOUCH_MATCH_OK);
  CHECK (direct (other, ref, &b) == 0);
  dd = kpt_verify (&dtpl, &b);
  kpc_free (&b);
  CHECK (d.score == dd.score && d.accept == dd.accept);
  printf ("other synthetic score %d\n", d.score);

  /* the reference is subtracted: the wrong reference changes the features */
  {
    static uint16_t zero[SIDETOUCH_PIXELS];

    CHECK (sidetouch_match_features (t[0], ref, &a) == SIDETOUCH_MATCH_OK);
    CHECK (sidetouch_match_features (t[0], zero, &b) == SIDETOUCH_MATCH_OK);
    CHECK (!same_set (&a, &b));
    kpc_free (&a);
    kpc_free (&b);
  }

  /* 16-bit samples are unsigned: a reference straddling 32768 must give what
   * the direct path gives */
  {
    static uint16_t high[SIDETOUCH_PIXELS], hi_touch[SIDETOUCH_PIXELS];

    for (int k = 0; k < SIDETOUCH_PIXELS; k++)
      high[k] = (uint16_t) (32700 + (k % SIDETOUCH_COLS) * 2);
    blobs (1);
    touch (hi_touch, high, 0.0, 0.0);
    CHECK (sidetouch_match_features (hi_touch, high, &a) == SIDETOUCH_MATCH_OK);
    CHECK (direct (hi_touch, high, &b) == 0);
    CHECK (same_set (&a, &b));
    kpc_free (&a);
    kpc_free (&b);
  }

  /* one round blob: two keypoints at one location, which the candidate
   * calls unscorable although the set is not empty */
  for (int r = 0; r < SIDETOUCH_ROWS; r++)
    for (int c = 0; c < SIDETOUCH_COLS; c++)
      {
        double ex = c - 56.0, ey = r - 57.0;

        single[r * SIDETOUCH_COLS + c] = (uint16_t) lround (ref[r * SIDETOUCH_COLS + c] - 300.0 * exp (-(ex * ex + ey * ey) / 18.0));
      }
  CHECK (direct (single, ref, &b) == -1);
  CHECK (sidetouch_match_features (single, ref, &a) == SIDETOUCH_MATCH_UNSCORABLE && a.k == NULL);
  kpc_free (&b);

  /* an unscorable probe: rejecting decision */
  d.score = 99;
  d.accept = 1;
  CHECK (sidetouch_match_verify (&tpl, ref, flat, &d) == SIDETOUCH_MATCH_UNSCORABLE);
  CHECK (d.score == 0 && !d.accept);
  CHECK (sidetouch_match_features (flat, ref, &a) == SIDETOUCH_MATCH_UNSCORABLE && a.k == NULL && a.count == 0);
  kpt_free (&tpl);
  kpt_free (&dtpl);

  /* unscorable touches are dropped down to three members, then refused */
  touches[1] = single;
  touches[3] = flat;
  CHECK (sidetouch_match_enrol (touches, ref, &tpl, &dropped) == SIDETOUCH_MATCH_OK);
  CHECK (dropped == 2 && tpl.members == 3);
  /* members keep touch order: the second member is touch 3 */
  CHECK (sidetouch_match_features (t[2], ref, &a) == SIDETOUCH_MATCH_OK);
  CHECK (same_set (&tpl.member[1], &a));
  kpc_free (&a);
  kpt_free (&tpl);
  touches[4] = flat;
  CHECK (sidetouch_match_enrol (touches, ref, &tpl, &dropped) == SIDETOUCH_MATCH_TOO_FEW);
  CHECK (dropped == 3 && tpl.members == 0);

  /* invalid arguments */
  CHECK (sidetouch_match_features (NULL, ref, &a) == SIDETOUCH_MATCH_INVALID);
  CHECK (sidetouch_match_features (t[0], NULL, &a) == SIDETOUCH_MATCH_INVALID);
  CHECK (sidetouch_match_features (t[0], ref, NULL) == SIDETOUCH_MATCH_INVALID);
  touches[2] = NULL;
  CHECK (sidetouch_match_enrol (touches, ref, &tpl, NULL) == SIDETOUCH_MATCH_INVALID);
  CHECK (sidetouch_match_verify (NULL, ref, probe, &d) == SIDETOUCH_MATCH_INVALID);
  CHECK (sidetouch_match_verify (&tpl, ref, probe, &d) == SIDETOUCH_MATCH_INVALID);   /* no members */

  CHECK_DONE ("test-sidetouch-match");
}
