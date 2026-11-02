/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Script builders for the SideTouch state-machine tests (synthetic data). */
#pragma once

#include "sidetouch-fake.h"

typedef struct
{
  GArray    *steps;
  GPtrArray *keep;   /* bytes the steps point at */
} Script;

void script_init (Script *s);
void script_clear (Script *s);
void step (Script *s, FakeStep st);
void out (Script *s, const guint8 *data, gsize length);
void in (Script *s, const guint8 *data, gsize length);

#define OUT(s, b) out ((s), (b), sizeof (b))
#define IN(s, b) in ((s), (b), sizeof (b))

/* Length request, length reply, data request, then body (the whole read). */
void serial_read (Script *s, const guint8 *body, gsize length);

/* One Eagle exchange with sequence seq: preflight, request, acknowledgement,
 * poll, and a valid response carrying payload. */
void exchange (Script *s, guint16 seq, const guint8 *cmd, gsize cmd_length, const guint8 *payload, gsize length);

#define EXCHANGE(s, seq, cmd, ...) do { \
    const guint8 p_[] = { __VA_ARGS__ }; \
    exchange ((s), (seq), (cmd), sizeof (cmd), p_, sizeof p_); \
  } while (0)

/* The established bridge initialisation, with one sweep pass that completes. */
void bridge_ok (Script *s);

/* Whole transactions. Each returns the next sequence number. */

/* Bridge initialisation and the firmware query (5.9). */
guint16 script_open (Script *s, guint16 seq);

/* A touch-mode acquisition delivering stream (13,696 bytes), with the given
 * 49-byte settings and status word 0x001e. */
guint16 script_touch (Script *s, guint16 seq, const guint8 *settings, const guint8 *stream);

/* An untouched acquisition delivering stream, status 0x005a / 0x001a. */
guint16 script_untouched (Script *s, guint16 seq, const guint8 *stream);

/* The same with the given 01 43 and 01 49 words. */
guint16 script_untouched_status (Script *s, guint16 seq, const guint8 *stream, guint16 status, guint16 ready);

/* An untouched acquisition whose 01 43 word the reader refuses: it ends
 * after the clear, with no data-ready query or range read. */
guint16 script_untouched_refused (Script *s, guint16 seq, guint16 status);

/* A touch-mode acquisition that waits (one fa) and is then stopped through
 * the cancellation sequence. The wait step calls before() first, which
 * should cancel or suspend. With late, one more poll is sent (and cancelled)
 * before the stop takes effect: the core delivers a user cancellation to the
 * driver from an idle callback, a suspend at once. */
guint16 script_touch_cancelled (Script *s, guint16 seq, const guint8 *settings, void (*before) (gpointer),
                                gboolean late);

/* The 1.5 s pause before the reference frames. */
void script_pause (Script *s);

/* Valid 49-byte runtime information; variant changes a compared byte. */
void script_settings (guint8 out[49], int variant);
