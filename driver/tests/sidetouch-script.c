/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Script builders for the SideTouch state-machine tests. See sidetouch-script.h. */
#include "sidetouch-script.h"
#include "sidetouch-proto.h"

#include <string.h>

static const guint8 poll[] = { 0x81 };
static const guint8 st_wait[] = { 0x32, 0x60, 0xfa };
static const guint8 st_ready[] = { 0x32, 0x60, 0xda };
static const guint8 ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62 };


void
script_init (Script *s)
{
  s->steps = g_array_new (FALSE, TRUE, sizeof (FakeStep));
  s->keep = g_ptr_array_new_with_free_func (g_free);
}

void
script_clear (Script *s)
{
  g_array_unref (s->steps);
  g_ptr_array_unref (s->keep);
}

static const guint8 *
keep (Script *s, const guint8 *data, gsize length)
{
  guint8 *copy = g_memdup2 (data, length);

  g_ptr_array_add (s->keep, copy);
  return copy;
}

void
step (Script *s, FakeStep st)
{
  g_array_append_val (s->steps, st);
}

void
out (Script *s, const guint8 *data, gsize length)
{
  step (s, (FakeStep) { .kind = FAKE_SEND, .data = keep (s, data, length), .length = length });
}

void
in (Script *s, const guint8 *data, gsize length)
{
  step (s, (FakeStep) { .kind = FAKE_RECEIVE, .data = keep (s, data, length), .length = length });
}


void
serial_read (Script *s, const guint8 *body, gsize length)
{
  guint8 frame[SIDETOUCH_USB_PACKET];
  guint8 reply[5] = { 0x32, 0x60, 0x62, (guint8) (length - 3), (guint8) ((length - 3) >> 8) };
  gsize n;

  n = sidetouch_serial_length_request (frame, sizeof frame);
  out (s, frame, n);
  IN (s, reply);
  n = sidetouch_serial_data_request ((uint16_t) (length - 3), frame, sizeof frame);
  out (s, frame, n);
  in (s, body, length);
}

/* One Eagle exchange: request with sequence seq, response payload. */
void
exchange (Script *s, guint16 seq, const guint8 *cmd, gsize cmd_length, const guint8 *payload, gsize length)
{
  guint8 frame[SIDETOUCH_USB_PACKET];
  guint8 r[SIDETOUCH_USB_PACKET];
  gsize n = sidetouch_eagle_request (seq, cmd, cmd_length, frame, sizeof frame);
  gsize announced = 2 + 4 + 8 + length;
  guint8 *e = r + 9;
  uint16_t crc;

  g_assert_cmpuint (n, >, 0);
  OUT (s, poll); IN (s, st_wait); OUT (s, poll); IN (s, st_wait);
  out (s, frame, n);
  IN (s, ack);
  OUT (s, poll); IN (s, st_ready);

  r[0] = 0x32; r[1] = 0x60; r[2] = 0x62;
  r[3] = (guint8) announced; r[4] = (guint8) (announced >> 8);
  r[5] = 0x1f; r[6] = 0x00; r[7] = 0x0b; r[8] = 0x00;
  e[0] = 0xca; e[1] = 0xf0; e[2] = (guint8) seq; e[3] = (guint8) (seq >> 8);
  e[4] = (guint8) (8 + length); e[5] = (guint8) ((8 + length) >> 8); e[6] = 0; e[7] = 0;
  memcpy (e + 8, payload, length);
  crc = sidetouch_crc16 (e, 8 + length);
  e[6] = (guint8) crc; e[7] = (guint8) (crc >> 8);
  serial_read (s, r, 3 + announced);
}



void
bridge_ok (Script *s)
{
  static const guint8 two[] = { 0x32, 0x60 };
  static const guint8 setup_1[] = { 0x8a };
  static const guint8 setup_2[] = { 0x86, 0x03, 0x00 };
  static const guint8 setup_3[] = { 0x9e, 0x00, 0x40 };
  static const guint8 setup_4[] = { 0x80, 0x00, 0x03, 0x82, 0x42, 0x42 };
  static const guint8 setup_5[] = { 0x80, 0x42, 0x03, 0x82, 0x42, 0x42 };
  static const guint8 seven[] = {
    0x82, 0x40, 0x42, 0x31, 0x07, 0x00, 0x00, 0x04, 0x00, 0x05, 0x00, 0x2f, 0x00, 0x00, 0x82, 0x42, 0x42,
  };
  static const guint8 seven_ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62 };
  static const guint8 st_done[] = { 0x32, 0x60, 0x20 };
  static const guint8 st_idle[] = { 0x32, 0x60, 0x00 };
  static const guint8 final_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x1f, 0x00, 0x80 };
  const struct { guint8 r; guint16 v, i; gboolean drain; } c[] = {
    { 0x00, 0, 0, 0 }, { 0x00, 0, 1, 0 }, { 0x00, 0, 2, 1 },
    { 0x00, 0, 0, 0 }, { 0x06, 0, 0, 0 }, { 0x07, 0, 0, 1 },
    { 0x09, 1, 0, 0 }, { 0x0b, 0, 0, 1 },
    { 0x0b, 0x200, 0, 1 },
    { 0x00, 0, 0, 0 },
  };

  for (gsize k = 0; k < G_N_ELEMENTS (c); k++)
    {
      step (s, (FakeStep) { .kind = FAKE_CONTROL, .request = c[k].r, .value = c[k].v, .index = c[k].i });
      if (c[k].drain)
        IN (s, two);
    }
  OUT (s, setup_1); OUT (s, setup_2);
  step (s, (FakeStep) { .kind = FAKE_CONTROL });
  OUT (s, setup_3); OUT (s, setup_4); OUT (s, setup_5);
  OUT (s, poll); IN (s, st_idle);
  OUT (s, poll); IN (s, st_done);
  OUT (s, seven); IN (s, seven_ack);
  OUT (s, poll); IN (s, st_idle);
  serial_read (s, final_5, sizeof final_5);
}

static const guint8 cmd_fw[] = { 0x01, 0x42 };
static const guint8 cmd_info[] = { 0x01, 0x41 };
static const guint8 cmd_f[] = { 0x02, 0x46, 0x19, 0x00, 0x00, 0x00 };
static const guint8 cmd_e_touch[] = { 0x02, 0x45, 0x80, 0x96, 0x98, 0x00 };
static const guint8 cmd_e_untouched[] = { 0x02, 0x45, 0x30, 0x75, 0x00, 0x00 };
static const guint8 cmd_touch[] = { 0x08, 0x07 };
static const guint8 cmd_untouched[] = { 0x08, 0x05 };
static const guint8 cmd_status[] = { 0x01, 0x43 };
static const guint8 cmd_clear[] = { 0x02, 0x43, 0xff, 0xff, 0x00 };
static const guint8 cmd_ready[] = { 0x01, 0x49 };
static const guint8 drain[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x07, 0x01 };

guint16
script_open (Script *s, guint16 seq)
{
  bridge_ok (s);
  EXCHANGE (s, seq, cmd_fw, 0x00, 0x05, 0x09, 0x68, 0x00);
  return (guint16) (seq + 1);
}

void
script_settings (guint8 out[49], int variant)
{
  const struct { int at; unsigned v; } f[] = {
    { 1, 113 }, { 3, 115 }, { 5, 373 }, { 7, 8 }, { 11, 0x45b0 }, { 13, 0x3580 }, { 15, 1 }, { 27, 4 },
  };

  memset (out, 0, 49);
  for (gsize i = 0; i < G_N_ELEMENTS (f); i++)
    {
      out[f[i].at] = (guint8) f[i].v;
      out[f[i].at + 1] = (guint8) (f[i].v >> 8);
    }
  out[0x11] = (guint8) (0xc3 + variant);   /* a compared byte outside the layout fields */
}

static guint16
setup (Script *s, guint16 seq, gboolean touch, const guint8 *settings)
{
  if (touch)
    exchange (s, seq++, cmd_info, sizeof cmd_info, settings, 49);
  EXCHANGE (s, seq++, cmd_f, 0x00);
  if (touch)
    {
      EXCHANGE (s, seq++, cmd_e_touch, 0x00);
      EXCHANGE (s, seq++, cmd_touch, 0x00);
    }
  else
    {
      EXCHANGE (s, seq++, cmd_e_untouched, 0x00);
      EXCHANGE (s, seq++, cmd_untouched, 0x00);
    }
  return seq;
}

static guint16
readout (Script *s, guint16 seq, guint16 status, guint16 ready, const guint8 *stream)
{
  OUT (s, poll); IN (s, st_wait);
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50 });
  OUT (s, poll); IN (s, st_ready);
  serial_read (s, drain, sizeof drain);
  EXCHANGE (s, seq++, cmd_status, 0x00, (guint8) status, (guint8) (status >> 8), 0x00);
  EXCHANGE (s, seq++, cmd_clear, 0x00);
  EXCHANGE (s, seq++, cmd_ready, 0x00, (guint8) ready, (guint8) (ready >> 8));
  for (gsize off = 0; off < SIDETOUCH_ACQUISITION_LENGTH; off += SIDETOUCH_CHUNK_LENGTH)
    {
      gsize n = MIN ((gsize) SIDETOUCH_CHUNK_LENGTH, SIDETOUCH_ACQUISITION_LENGTH - off);
      guint8 cmd[5], payload[1 + SIDETOUCH_CHUNK_LENGTH];

      sidetouch_cmd_range ((uint16_t) off, (uint16_t) n, cmd, sizeof cmd);
      payload[0] = 0;
      memcpy (payload + 1, stream + off, n);
      exchange (s, seq++, cmd, sizeof cmd, payload, n + 1);
    }
  return seq;
}

guint16
script_touch (Script *s, guint16 seq, const guint8 *settings, const guint8 *stream)
{
  return readout (s, setup (s, seq, TRUE, settings), 0x001e, 0x001e, stream);
}

guint16
script_untouched (Script *s, guint16 seq, const guint8 *stream)
{
  return readout (s, setup (s, seq, FALSE, NULL), 0x005a, 0x001a, stream);
}

guint16
script_untouched_status (Script *s, guint16 seq, const guint8 *stream, guint16 status, guint16 ready)
{
  return readout (s, setup (s, seq, FALSE, NULL), status, ready, stream);
}

guint16
script_untouched_refused (Script *s, guint16 seq, guint16 status)
{
  seq = setup (s, seq, FALSE, NULL);
  OUT (s, poll); IN (s, st_wait);
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50 });
  OUT (s, poll); IN (s, st_ready);
  serial_read (s, drain, sizeof drain);
  EXCHANGE (s, seq++, cmd_status, 0x00, (guint8) status, (guint8) (status >> 8), 0x00);
  EXCHANGE (s, seq++, cmd_clear, 0x00);
  return seq;
}

guint16
script_touch_cancelled (Script *s, guint16 seq, const guint8 *settings, void (*before) (gpointer), gboolean late)
{
  static const guint8 cancelled[] = { 0x00, 0x4a, 0x00, 0x13 };
  static const guint8 st_d8[] = { 0x32, 0x60, 0xd8 };
  guint8 frame[SIDETOUCH_USB_PACKET];
  gsize n;

  seq = setup (s, seq, TRUE, settings);
  OUT (s, poll); IN (s, st_wait);
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50, .before = before });
  if (late)
    OUT (s, poll);
  n = sidetouch_eagle_request (seq++, NULL, 0, frame, sizeof frame);
  out (s, frame, n);
  IN (s, ack);
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 60 });
  OUT (s, poll); IN (s, st_d8);
  serial_read (s, drain, sizeof drain);
  exchange (s, seq++, cmd_status, sizeof cmd_status, cancelled, sizeof cancelled);
  EXCHANGE (s, seq++, cmd_clear, 0x00);
  return seq;
}

void
script_pause (Script *s)
{
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 1500 });
}
