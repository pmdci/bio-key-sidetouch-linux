/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Synthetic format-4 streams. See sidetouch-synth.h. */
#include "sidetouch-synth.h"

#include <string.h>

static void
put_u16 (uint8_t *p, unsigned v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

int
synth_encode (const uint16_t g[SIDETOUCH_PIXELS], uint8_t out[SIDETOUCH_FORMAT4_LENGTH])
{
  uint8_t *p = out + 20;

  memset (out, 0, SIDETOUCH_FORMAT4_LENGTH);
  put_u16 (out, 0xea61);
  put_u16 (out + 2, 4);
  put_u16 (out + 6, 58);
  put_u16 (out + 8, 58);
  put_u16 (out + 10, 57);
  put_u16 (out + 14, 113);
  put_u16 (out + 4, 0x1234);   /* words the decoder does not use */
  put_u16 (out + 18, 0xbeef);
  for (int r = 0; r < SIDETOUCH_ROWS; r++, p += 2)
    put_u16 (p, g[r * SIDETOUCH_COLS]);
  for (int col = 1; col < SIDETOUCH_COLS; col++)
    {
      int absolute = col >= 58 && col <= 60;

      for (int r = 0; r < SIDETOUCH_ROWS; r++)
        {
          unsigned v = g[r * SIDETOUCH_COLS + col];

          if (absolute || r == 57 || r == 58)
            {
              put_u16 (p, v);
              p += 2;
            }
          else
            {
              int d = (int) v - (int) g[r * SIDETOUCH_COLS + col - 1];

              if (d < -128 || d > 127)
                return -1;
              *p++ = (uint8_t) (int8_t) d;
            }
        }
    }
  return p == out + 0x357d ? 0 : -1;
}

void
synth_grid (uint16_t g[SIDETOUCH_PIXELS], unsigned seed)
{
  unsigned x = seed * 2654435761u + 1;

  for (int r = 0; r < SIDETOUCH_ROWS; r++)
    {
      int v = 400 + r * 3;

      for (int c = 0; c < SIDETOUCH_COLS; c++)
        {
          x = x * 1103515245u + 12345u;
          v += (int) ((x >> 16) % 61) - 30;
          if (v < 40)
            v = 40;
          if (v > 1700)
            v = 1700;
          g[r * SIDETOUCH_COLS + c] = (uint16_t) v;
        }
    }
  /* absolute rows and columns may jump arbitrarily */
  g[57 * SIDETOUCH_COLS + 80] = 1;
  g[57 * SIDETOUCH_COLS + 81] = 1;
  g[59 * SIDETOUCH_COLS + 59] = 60000;
}
