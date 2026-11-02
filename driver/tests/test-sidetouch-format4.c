/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-format4 on synthetic streams: encode, decode, compare. */
#include "check.h"
#include "sidetouch-format4.h"
#include "sidetouch-synth.h"

int
main (void)
{
  static uint16_t g[SIDETOUCH_PIXELS], out[SIDETOUCH_PIXELS];
  static uint8_t s[SIDETOUCH_FORMAT4_LENGTH], t[SIDETOUCH_FORMAT4_LENGTH];

  for (unsigned seed = 1; seed <= 5; seed++)
    {
      synth_grid (g, seed);
      CHECK (synth_encode (g, s) == 0);
      CHECK (sidetouch_format4_decode (s, sizeof s, out) == SIDETOUCH_FORMAT4_OK);
      CHECK_MEM (out, g, sizeof g);
    }

  CHECK (sidetouch_format4_decode (s, sizeof s - 1, out) == SIDETOUCH_FORMAT4_LENGTH_ERROR);
  CHECK (sidetouch_format4_decode (NULL, sizeof s, out) == SIDETOUCH_FORMAT4_LENGTH_ERROR);
  int offsets[] = { 0, 2, 6, 8, 10, 14 };
  for (size_t i = 0; i < sizeof offsets / sizeof *offsets; i++)
    {
      memcpy (t, s, sizeof t);
      t[offsets[i]] ^= 1;
      CHECK (sidetouch_format4_decode (t, sizeof t, out) == SIDETOUCH_FORMAT4_HEADER);
    }
  for (int i = 0; i < 3; i++)
    {
      memcpy (t, s, sizeof t);
      t[0x357d + i] = 1;
      CHECK (sidetouch_format4_decode (t, sizeof t, out) == SIDETOUCH_FORMAT4_TRAILER);
    }
  CHECK_DONE ("test-sidetouch-format4");
}
