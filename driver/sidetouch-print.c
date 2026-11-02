/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The SideTouch print container. See sidetouch-print.h.
 */
#include "sidetouch-print.h"
#include "sidetouch-wipe.h"

#include <string.h>

static const uint8_t magic[4] = { 'S', 'T', 'P', 'R' };

#define REFERENCE_BYTES (2 * SIDETOUCH_PIXELS)
#define FIXED_BYTES (4 + 2 + SIDETOUCH_SETTINGS_LENGTH + REFERENCE_BYTES + 4)

/* Compared settings ranges; see sidetouch-print.h. */
static const struct { size_t start, end; } compared[] = {
  { 0x01, 0x0d },
  { 0x11, 0x19 },
};

static void
put_u16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

static uint16_t
get_u16 (const uint8_t *p)
{
  return (uint16_t) (p[0] | (p[1] << 8));
}

static void
put_u32 (uint8_t *p, uint32_t v)
{
  put_u16 (p, (uint16_t) v);
  put_u16 (p + 2, (uint16_t) (v >> 16));
}

static uint32_t
get_u32 (const uint8_t *p)
{
  return (uint32_t) get_u16 (p) | ((uint32_t) get_u16 (p + 2) << 16);
}

size_t
sidetouch_print_serialised_size (const SidetouchPrint *print)
{
  size_t t;

  if (!print || print->template.members < KPT_MIN_MEMBERS || print->template.members > KPT_MAX_MEMBERS)
    return 0;
  t = kpt_serialised_size (&print->template);
  if (t > UINT32_MAX)
    return 0;
  return FIXED_BYTES + t;
}

size_t
sidetouch_print_serialise (const SidetouchPrint *print, uint8_t *out, size_t capacity)
{
  size_t total = sidetouch_print_serialised_size (print), t;
  uint8_t *p;

  if (!total || !out || total > capacity)
    return 0;
  p = out;
  memcpy (p, magic, sizeof magic);
  put_u16 (p + 4, SIDETOUCH_PRINT_VERSION);
  memcpy (p + 6, print->settings, SIDETOUCH_SETTINGS_LENGTH);
  p += 6 + SIDETOUCH_SETTINGS_LENGTH;
  for (int k = 0; k < SIDETOUCH_PIXELS; k++, p += 2)
    put_u16 (p, print->reference[k]);
  t = total - FIXED_BYTES;
  put_u32 (p, (uint32_t) t);
  p += 4;
  if (kpt_serialise (&print->template, p, t) != t)
    {
      memset (out, 0, total);
      return 0;
    }
  return total;
}

SidetouchPrintResult
sidetouch_print_deserialise (const uint8_t *data, size_t length, SidetouchPrint *out)
{
  const uint8_t *p;

  if (!out)
    return SIDETOUCH_PRINT_MALFORMED;
  memset (out, 0, sizeof *out);
  if (!data || length < FIXED_BYTES || memcmp (data, magic, sizeof magic) != 0)
    return SIDETOUCH_PRINT_MALFORMED;
  if (get_u16 (data + 4) != SIDETOUCH_PRINT_VERSION)
    return SIDETOUCH_PRINT_VERSION_ERROR;
  p = data + 6;
  memcpy (out->settings, p, SIDETOUCH_SETTINGS_LENGTH);
  p += SIDETOUCH_SETTINGS_LENGTH;
  for (int k = 0; k < SIDETOUCH_PIXELS; k++, p += 2)
    out->reference[k] = get_u16 (p);
  if (get_u32 (p) != length - FIXED_BYTES)
    {
      sidetouch_print_clear (out);
      return SIDETOUCH_PRINT_MALFORMED;
    }
  p += 4;
  /* kpt_deserialise bounds every count against the remaining length and
   * rejects trailing bytes; it cannot tell a malformed template from an
   * allocation failure, so both are reported as malformed. */
  if (kpt_deserialise (p, length - FIXED_BYTES, &out->template) != 0)
    {
      sidetouch_print_clear (out);
      return SIDETOUCH_PRINT_MALFORMED;
    }
  return SIDETOUCH_PRINT_OK;
}

void
sidetouch_print_clear (SidetouchPrint *print)
{
  if (!print)
    return;
  sidetouch_wipe_template (&print->template);
  sidetouch_wipe (print->settings, sizeof print->settings);
  sidetouch_wipe (print->reference, sizeof print->reference);
}

bool
sidetouch_settings_match (const uint8_t enrolled[SIDETOUCH_SETTINGS_LENGTH],
                          const uint8_t current[SIDETOUCH_SETTINGS_LENGTH])
{
  if (!enrolled || !current)
    return false;
  for (size_t i = 0; i < sizeof compared / sizeof *compared; i++)
    if (memcmp (enrolled + compared[i].start, current + compared[i].start,
                compared[i].end - compared[i].start) != 0)
      return false;
  return true;
}
