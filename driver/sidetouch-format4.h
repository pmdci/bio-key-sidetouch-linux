/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The SideTouch format-4 acquisition: a 13,696-byte predictive encoding of a
 * 115-row by 113-column grid of unsigned 16-bit sensor samples.
 *
 * A 20-byte header (ten little-endian words) gives the partition sizes; the
 * body holds column 0 as absolute samples, then 57 delta columns, three
 * absolute columns and 52 delta columns. A delta column stores each sample as
 * a signed byte relative to the sample to its left, except for two absolute
 * rows (rows 57 and 58). Three zero bytes follow the body. The decoder accepts
 * only that exact established layout and treats the input as untrusted.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SIDETOUCH_ROWS 115
#define SIDETOUCH_COLS 113
#define SIDETOUCH_PIXELS (SIDETOUCH_ROWS * SIDETOUCH_COLS)
#define SIDETOUCH_FORMAT4_LENGTH 13696

typedef enum {
  SIDETOUCH_FORMAT4_OK = 0,
  SIDETOUCH_FORMAT4_LENGTH_ERROR = -1,
  SIDETOUCH_FORMAT4_HEADER = -2,
  SIDETOUCH_FORMAT4_TRAILER = -3,
} SidetouchFormat4Result;

/* Decodes into row-major samples. On error the output is unspecified. */
SidetouchFormat4Result sidetouch_format4_decode (const uint8_t *data,
                                                 size_t         length,
                                                 uint16_t       samples[SIDETOUCH_PIXELS]);
