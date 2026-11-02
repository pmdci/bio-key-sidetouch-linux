/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * SideTouch format-4 decoder. See sidetouch-format4.h.
 */
#include "sidetouch-format4.h"

#define HEADER_BYTES 20
#define SIGNATURE 0xea61
#define ROWS_BEFORE 57     /* delta rows before the two absolute rows */
#define ABSOLUTE_ROWS 2
#define COLS_BEFORE 57     /* delta columns before the three absolute columns */
#define ABSOLUTE_COLS 3
#define BODY_END 0x357d    /* header plus body; three zero bytes follow */

static uint16_t
get_u16 (const uint8_t *p)
{
  return (uint16_t) (p[0] | (p[1] << 8));
}

/* Layout words: [0] signature, [1] format, [3] rows before + 1, [4] delta
 * columns before + 1, [5] rows after + 1, [7] columns. The rest are not used. */
static int
header_ok (const uint8_t *h)
{
  return get_u16 (h) == SIGNATURE && get_u16 (h + 2) == 4 &&
         get_u16 (h + 6) == ROWS_BEFORE + 1 && get_u16 (h + 8) == COLS_BEFORE + 1 &&
         get_u16 (h + 10) == SIDETOUCH_ROWS - ROWS_BEFORE - ABSOLUTE_ROWS + 1 &&
         get_u16 (h + 14) == SIDETOUCH_COLS;
}

static void
delta_column (const uint8_t **p, uint16_t *s, int col)
{
  for (int row = 0; row < SIDETOUCH_ROWS; row++)
    {
      uint16_t *v = &s[row * SIDETOUCH_COLS + col];

      if (row >= ROWS_BEFORE && row < ROWS_BEFORE + ABSOLUTE_ROWS)
        {
          *v = get_u16 (*p);
          *p += 2;
        }
      else
        {
          int8_t d = (int8_t) **p;

          *v = (uint16_t) (v[-1] + d);
          *p += 1;
        }
    }
}

SidetouchFormat4Result
sidetouch_format4_decode (const uint8_t *data, size_t length, uint16_t samples[SIDETOUCH_PIXELS])
{
  const uint8_t *p;
  int col;

  /* The exact layout fixes the length, so nothing below can read past the end. */
  if (!data || !samples || length != SIDETOUCH_FORMAT4_LENGTH)
    return SIDETOUCH_FORMAT4_LENGTH_ERROR;
  if (!header_ok (data))
    return SIDETOUCH_FORMAT4_HEADER;
  if (data[BODY_END] || data[BODY_END + 1] || data[BODY_END + 2])
    return SIDETOUCH_FORMAT4_TRAILER;

  p = data + HEADER_BYTES;
  for (int row = 0; row < SIDETOUCH_ROWS; row++, p += 2)
    samples[row * SIDETOUCH_COLS] = get_u16 (p);
  for (col = 1; col <= COLS_BEFORE; col++)
    delta_column (&p, samples, col);
  for (; col <= COLS_BEFORE + ABSOLUTE_COLS; col++)
    for (int row = 0; row < SIDETOUCH_ROWS; row++, p += 2)
      samples[row * SIDETOUCH_COLS + col] = get_u16 (p);
  for (; col < SIDETOUCH_COLS; col++)
    delta_column (&p, samples, col);

  return p == data + BODY_END ? SIDETOUCH_FORMAT4_OK : SIDETOUCH_FORMAT4_LENGTH_ERROR;
}
