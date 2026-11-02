/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-print on synthetic prints (no biometric data). */
#include <math.h>
#include <stdint.h>

#include "check.h"
#include "sidetouch-print.h"

static SidetouchPrint a, b;

static void
synthetic (SidetouchPrint *p)
{
  static Keypoint k[KPT_MAX_MEMBERS][4];
  KpSet sets[KPT_MAX_MEMBERS];
  const KpSet *in[KPT_MAX_MEMBERS];

  for (int m = 0; m < KPT_MAX_MEMBERS; m++)
    {
      for (int i = 0; i < 4; i++)
        {
          k[m][i].x = 10.5 + m + i;
          k[m][i].y = 20.25 * (i + 1);
          k[m][i].sigma = 1.6 + 0.1 * i;
          k[m][i].theta = -1.0 + 0.5 * m;
          for (int d = 0; d < KP_DESCRIPTOR; d++)
            k[m][i].d[d] = (unsigned char) (m * 31 + i * 7 + d);
        }
      sets[m].k = k[m];
      sets[m].count = (size_t) (2 + m % 3);
      in[m] = &sets[m];
    }
  memset (p, 0, sizeof *p);
  for (int i = 0; i < SIDETOUCH_SETTINGS_LENGTH; i++)
    p->settings[i] = (uint8_t) (0x80 + i);
  for (int i = 0; i < SIDETOUCH_PIXELS; i++)
    p->reference[i] = (uint16_t) ((unsigned) i * 37u + 11u);
  p->reference[0] = 0;
  p->reference[1] = 65535;
  CHECK (kpt_enrol (in, KPT_MAX_MEMBERS, &p->template) == 0);
}

static int
same_print (const SidetouchPrint *x, const SidetouchPrint *y)
{
  if (memcmp (x->settings, y->settings, sizeof x->settings) ||
      memcmp (x->reference, y->reference, sizeof x->reference) ||
      x->template.members != y->template.members)
    return 0;
  for (int m = 0; m < x->template.members; m++)
    {
      const KpSet *s = &x->template.member[m], *t = &y->template.member[m];

      if (s->count != t->count || memcmp (s->k, t->k, s->count * sizeof *s->k))
        return 0;
    }
  return 1;
}

int
main (void)
{
  size_t n, n2;
  uint8_t *buf, *buf2, *t;

  synthetic (&a);
  n = sidetouch_print_serialised_size (&a);
  CHECK (n == 4 + 2 + 49 + 2 * SIDETOUCH_PIXELS + 4 + kpt_serialised_size (&a.template));
  buf = malloc (n + 1);
  buf2 = malloc (n + 1);
  t = malloc (n + 1);
  CHECK (buf && buf2 && t);
  if (!buf || !buf2 || !t)
    CHECK_DONE ("test-sidetouch-print");

  /* round trip, byte-exact on re-serialisation */
  CHECK (sidetouch_print_serialise (&a, buf, n - 1) == 0);
  CHECK (sidetouch_print_serialise (&a, buf, n) == n);
  CHECK (memcmp (buf, "STPR\x01\x00", 6) == 0);
  CHECK (buf[6] == 0x80 && buf[6 + 48] == 0x80 + 48);
  CHECK (sidetouch_print_deserialise (buf, n, &b) == SIDETOUCH_PRINT_OK);
  CHECK (same_print (&a, &b));
  n2 = sidetouch_print_serialise (&b, buf2, n);
  CHECK (n2 == n && memcmp (buf, buf2, n) == 0);
  sidetouch_print_clear (&b);
  CHECK (b.template.members == 0 && b.reference[1] == 0 && b.settings[3] == 0);

  /* every truncation is refused, as is one trailing byte */
  for (size_t len = 0; len < n; len += (len < 26000 ? 97 : 1))
    {
      if (sidetouch_print_deserialise (buf, len, &b) != SIDETOUCH_PRINT_MALFORMED)
        {
          CHECK (0 /* truncation accepted */);
          fprintf (stderr, "  length %zu\n", len);
          sidetouch_print_clear (&b);
          break;
        }
    }
  memcpy (t, buf, n);
  t[n] = 0;
  CHECK (sidetouch_print_deserialise (t, n + 1, &b) == SIDETOUCH_PRINT_MALFORMED);

  /* header fields */
  memcpy (t, buf, n);
  t[0] = 'X';
  CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);
  memcpy (t, buf, n);
  t[4] = 2;
  CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_VERSION_ERROR);
  memcpy (t, buf, n);
  t[5] = 1;
  CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_VERSION_ERROR);

  /* the template length must equal the rest exactly */
  {
    size_t at = 6 + 49 + 2 * SIDETOUCH_PIXELS;

    memcpy (t, buf, n);
    t[at] ^= 1;
    CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);
    memcpy (t, buf, n);
    t[at + 3] = 0x80;
    CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);

    /* a corrupt template: wrong magic, too few members, a NaN coordinate */
    memcpy (t, buf, n);
    t[at + 4] = 'X';
    CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);
    memcpy (t, buf, n);
    t[at + 4 + 6] = 2;
    CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);
    memcpy (t, buf, n);
    {
      double nan = NAN;
      uint64_t bits;

      memcpy (&bits, &nan, sizeof bits);
      for (int i = 0; i < 8; i++)
        t[at + 4 + 7 + 2 + (size_t) i] = (uint8_t) (bits >> (8 * i));
    }
    CHECK (sidetouch_print_deserialise (t, n, &b) == SIDETOUCH_PRINT_MALFORMED);
    CHECK (b.template.members == 0);
  }

  CHECK (sidetouch_print_deserialise (NULL, n, &b) == SIDETOUCH_PRINT_MALFORMED);
  CHECK (sidetouch_print_deserialise (buf, n, NULL) == SIDETOUCH_PRINT_MALFORMED);

  /* a print whose template cannot be serialised is refused */
  b = a;
  b.template.members = 2;
  CHECK (sidetouch_print_serialised_size (&b) == 0 && sidetouch_print_serialise (&b, t, n) == 0);
  memset (&b, 0, sizeof b);

  /* settings: exactly bytes [0x01,0x0d) and [0x11,0x19) are compared */
  for (int i = 0; i < SIDETOUCH_SETTINGS_LENGTH; i++)
    {
      uint8_t s[SIDETOUCH_SETTINGS_LENGTH];
      int compared = (i >= 0x01 && i < 0x0d) || (i >= 0x11 && i < 0x19);

      memcpy (s, a.settings, sizeof s);
      s[i] ^= 0x01;
      if (sidetouch_settings_match (a.settings, s) != !compared)
        {
          CHECK (0 /* settings range wrong */);
          fprintf (stderr, "  byte 0x%02x\n", i);
        }
    }
  CHECK (sidetouch_settings_match (a.settings, a.settings));
  CHECK (!sidetouch_settings_match (NULL, a.settings));

  sidetouch_print_clear (&a);
  free (buf);
  free (buf2);
  free (t);
  CHECK_DONE ("test-sidetouch-print");
}
