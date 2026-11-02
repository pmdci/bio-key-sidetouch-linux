/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The SideTouch print: what one enrolment stores, as one byte string that the
 * device class wraps in the FpPrint. Pure serialisation, no I/O.
 *
 * Serialised form (little-endian), validated as untrusted input on reload:
 *   "STPR" | u16 version (1)
 *   | 49 bytes: the "01 41" runtime-information payload read at enrolment
 *   | 12,995 x u16: the per-print background reference, row-major
 *   | u32 template length | the keypoint template ("SKPT", kptemplate.h)
 *
 * Only two ranges of the 49 settings bytes are compared at verification,
 * [0x01,0x0d) and [0x11,0x19): the fields a stored background reference must
 * agree with (sensor geometry, resolution, sample depth and buffer sizes).
 * The rest, never compared across sessions, is stored for diagnosis only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kptemplate.h"
#include "sidetouch-format4.h"

#define SIDETOUCH_SETTINGS_LENGTH 49
#define SIDETOUCH_PRINT_VERSION 1

typedef enum {
  SIDETOUCH_PRINT_OK = 0,
  SIDETOUCH_PRINT_MALFORMED = -1,   /* magic, length, bounds or template */
  SIDETOUCH_PRINT_VERSION_ERROR = -2,
  SIDETOUCH_PRINT_NO_MEMORY = -3,
} SidetouchPrintResult;

/* About 26 KB; allocate it, do not put it on the stack. */
typedef struct {
  uint8_t    settings[SIDETOUCH_SETTINGS_LENGTH];
  uint16_t   reference[SIDETOUCH_PIXELS];
  KpTemplate template;
} SidetouchPrint;

/* The serialised length, or 0 if the template cannot be serialised. */
size_t sidetouch_print_serialised_size (const SidetouchPrint *print);

/* Writes the print and returns its length, or 0 if it does not fit or the
 * template cannot be serialised. */
size_t sidetouch_print_serialise (const SidetouchPrint *print,
                                  uint8_t              *out,
                                  size_t                capacity);

/* On success the caller owns out and releases it with sidetouch_print_clear;
 * on failure out holds nothing to release. */
SidetouchPrintResult sidetouch_print_deserialise (const uint8_t  *data,
                                                  size_t          length,
                                                  SidetouchPrint *out);

/* Frees the template and wipes the reference and settings. */
void sidetouch_print_clear (SidetouchPrint *print);

/* True if the compared settings ranges are identical. */
bool sidetouch_settings_match (const uint8_t enrolled[SIDETOUCH_SETTINGS_LENGTH],
                               const uint8_t current[SIDETOUCH_SETTINGS_LENGTH]);
