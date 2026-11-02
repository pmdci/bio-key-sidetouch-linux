/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Per-print background reference. See sidetouch-reference.h.
 */
#include "sidetouch-reference.h"

#include <stddef.h>

static double
frame_mean (const uint16_t *s)
{
  uint64_t sum = 0;

  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    sum += s[k];
  return (double) sum / SIDETOUCH_PIXELS;
}

SidetouchReferenceResult
sidetouch_reference_build (const uint16_t *const    frames[SIDETOUCH_REFERENCE_FRAMES],
                           uint16_t                 reference[SIDETOUCH_PIXELS],
                           SidetouchReferenceStats *stats)
{
  double lo, hi;
  int spread = 0;

  if (!frames || !reference || !stats)
    return SIDETOUCH_REFERENCE_INVALID;
  for (int f = 0; f < SIDETOUCH_REFERENCE_FRAMES; f++)
    if (!frames[f])
      return SIDETOUCH_REFERENCE_INVALID;

  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    {
      uint32_t sum = 0;
      int min = UINT16_MAX, max = 0;

      for (int f = 0; f < SIDETOUCH_REFERENCE_FRAMES; f++)
        {
          int v = frames[f][k];

          sum += (uint32_t) v;
          if (v < min)
            min = v;
          if (v > max)
            max = v;
        }
      /* round to nearest: a remainder of 2 rounds up, 1 rounds down */
      reference[k] = (uint16_t) ((sum + 1) / SIDETOUCH_REFERENCE_FRAMES);
      if (max - min > spread)
        spread = max - min;
    }

  lo = hi = frame_mean (frames[0]);
  for (int f = 1; f < SIDETOUCH_REFERENCE_FRAMES; f++)
    {
      double m = frame_mean (frames[f]);

      if (m < lo)
        lo = m;
      if (m > hi)
        hi = m;
    }

  stats->max_spread = spread;
  stats->mean_range = hi - lo;
  if (spread > SIDETOUCH_G2_MAX_SPREAD)
    return SIDETOUCH_REFERENCE_SPREAD;
  if (stats->mean_range > SIDETOUCH_G2_MAX_MEAN_RANGE)
    return SIDETOUCH_REFERENCE_MEAN_RANGE;
  return SIDETOUCH_REFERENCE_OK;
}

SidetouchReferenceResult
sidetouch_reference_check_level (const uint16_t           reference[SIDETOUCH_PIXELS],
                                 const uint16_t *const    touches[SIDETOUCH_ENROLL_TOUCHES],
                                 SidetouchReferenceStats *stats)
{
  double means[SIDETOUCH_ENROLL_TOUCHES];

  if (!reference || !touches || !stats)
    return SIDETOUCH_REFERENCE_INVALID;
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    {
      int64_t sum = 0;

      if (!touches[i])
        return SIDETOUCH_REFERENCE_INVALID;
      for (int k = 0; k < SIDETOUCH_PIXELS; k++)
        sum += (int64_t) reference[k] - (int64_t) touches[i][k];
      means[i] = (double) sum / SIDETOUCH_PIXELS;
    }

  /* insertion sort of five values; the median is the middle one */
  for (int i = 1; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    for (int j = i; j > 0 && means[j - 1] > means[j]; j--)
      {
        double t = means[j];

        means[j] = means[j - 1];
        means[j - 1] = t;
      }

  stats->level = means[SIDETOUCH_ENROLL_TOUCHES / 2];
  return stats->level >= SIDETOUCH_G3_MIN_LEVEL ? SIDETOUCH_REFERENCE_OK : SIDETOUCH_REFERENCE_LEVEL;
}
