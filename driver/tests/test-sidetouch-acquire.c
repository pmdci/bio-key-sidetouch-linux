/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-acquire over the scripted transport, with synthetic
 * streams (no sensor data). */
#include "drivers_api.h"
#include "sidetouch-acquire.h"
#include "sidetouch-fake.h"
#include "sidetouch-script.h"
#include "sidetouch-proto.h"
#include "sidetouch-synth.h"
#include "test-device-fake.h"

static const guint8 poll[] = { 0x81 };
static const guint8 pending[] = { 0x32, 0x60 };
static const guint8 st_wait[] = { 0x32, 0x60, 0xfa };
static const guint8 st_ready[] = { 0x32, 0x60, 0xda };
static const guint8 st_d8[] = { 0x32, 0x60, 0xd8 };
static const guint8 ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62 };
static const guint8 drain[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x07, 0x01 };
static const guint8 drain_bad[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x07, 0x02 };

static guint8 info_payload[SIDETOUCH_SETTINGS_BYTES];
static guint8 stream[SIDETOUCH_FORMAT4_LENGTH];
static guint16 grid[SIDETOUCH_PIXELS];

static void
make_fixtures (void)
{
  const struct { int at; unsigned v; } f[] = {
    { 1, 113 }, { 3, 115 }, { 5, 373 }, { 7, 8 }, { 11, 0x45b0 }, { 13, 0x3580 }, { 15, 1 }, { 27, 4 },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (f); i++)
    {
      info_payload[f[i].at] = (guint8) f[i].v;
      info_payload[f[i].at + 1] = (guint8) (f[i].v >> 8);
    }
  info_payload[40] = 0x5a;   /* an uncompared byte, to check the copy */
  synth_grid (grid, 7);
  g_assert_cmpint (synth_encode (grid, stream), ==, 0);
}

static const guint8 cmd_f[] = { 0x02, 0x46, 0x19, 0x00, 0x00, 0x00 };
static const guint8 cmd_e_touch[] = { 0x02, 0x45, 0x80, 0x96, 0x98, 0x00 };
static const guint8 cmd_e_untouched[] = { 0x02, 0x45, 0x30, 0x75, 0x00, 0x00 };
static const guint8 cmd_touch[] = { 0x08, 0x07 };
static const guint8 cmd_untouched[] = { 0x08, 0x05 };
static const guint8 cmd_info[] = { 0x01, 0x41 };
static const guint8 cmd_status[] = { 0x01, 0x43 };
static const guint8 cmd_clear[] = { 0x02, 0x43, 0xff, 0xff, 0x00 };
static const guint8 cmd_ready[] = { 0x01, 0x49 };

static guint16
setup (Script *s, guint16 seq, SidetouchMode mode)
{
  if (mode == SIDETOUCH_MODE_TOUCH)
    exchange (s, seq++, cmd_info, sizeof cmd_info, info_payload, sizeof info_payload);
  EXCHANGE (s, seq++, cmd_f, 0x00);
  if (mode == SIDETOUCH_MODE_TOUCH)
    EXCHANGE (s, seq++, cmd_e_touch, 0x00);
  else
    EXCHANGE (s, seq++, cmd_e_untouched, 0x00);
  if (mode == SIDETOUCH_MODE_TOUCH)
    EXCHANGE (s, seq++, cmd_touch, 0x00);
  else
    EXCHANGE (s, seq++, cmd_untouched, 0x00);
  return seq;
}

static void
wait_polls (Script *s, int pending_states)
{
  for (int i = 0; i < pending_states; i++)
    {
      OUT (s, poll); IN (s, pending); IN (s, st_wait);
      step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50 });
    }
  OUT (s, poll); IN (s, st_ready);
}

/* From the ready drain to the last range read, with the given status words. */
static guint16
readout (Script *s, guint16 seq, guint16 status, guint16 ready)
{
  serial_read (s, drain, sizeof drain);
  EXCHANGE (s, seq++, cmd_status, 0x00, (guint8) status, (guint8) (status >> 8), 0x00);
  EXCHANGE (s, seq++, cmd_clear, 0x00);
  EXCHANGE (s, seq++, cmd_ready, 0x00, (guint8) ready, (guint8) (ready >> 8));
  for (gsize off = 0; off < SIDETOUCH_FORMAT4_LENGTH; off += SIDETOUCH_CHUNK_LENGTH)
    {
      gsize n = MIN ((gsize) SIDETOUCH_CHUNK_LENGTH, SIDETOUCH_FORMAT4_LENGTH - off);
      guint8 cmd[5], payload[1 + SIDETOUCH_CHUNK_LENGTH];

      sidetouch_cmd_range ((uint16_t) off, (uint16_t) n, cmd, sizeof cmd);
      payload[0] = 0;
      memcpy (payload + 1, stream + off, n);
      exchange (s, seq++, cmd, sizeof cmd, payload, n + 1);
    }
  return seq;
}

/* ---- running ------------------------------------------------------------ */

typedef struct
{
  Fake                 fake;
  SidetouchAcquire    *acquire;
  gboolean             done;
  GError              *error;
  SidetouchReaderState reader;
  guint16              status;
  gboolean             have_samples;
  gboolean             samples_equal;
  gboolean             have_settings;
  gboolean             settings_equal;
  guint                armed;
  guint                contact;
} Run;

static void
on_armed (FpDevice *device, gpointer user_data)
{
  Run *r = user_data;

  (void) device;
  r->armed++;
}

static void
on_contact (FpDevice *device, gpointer user_data)
{
  Run *r = user_data;

  (void) device;
  r->contact++;
}

static void
on_done (FpDevice *device, const SidetouchAcquisition *result, GError *error, SidetouchReaderState reader,
         gpointer user_data)
{
  Run *r = user_data;

  (void) device;
  g_assert_false (r->done);
  r->done = TRUE;
  r->error = error;
  r->reader = reader;
  r->acquire = NULL;
  if (result)
    {
      r->status = result->status;
      r->have_samples = result->samples != NULL;
      r->samples_equal = result->samples && memcmp (result->samples, grid, sizeof grid) == 0;
      r->have_settings = result->settings != NULL;
      r->settings_equal = result->settings && memcmp (result->settings, info_payload, sizeof info_payload) == 0;
    }
}

static void
cancel_hook (gpointer data)
{
  Run *r = data;

  sidetouch_acquire_cancel (r->acquire);
}

static const SidetouchAcquireCallbacks callbacks = { on_armed, on_contact, on_done };

static void
run (Run *r, Script *s, SidetouchMode mode, guint16 *seq)
{
  SidetouchTransport transport = { &fake_ops, &r->fake };
  FpDevice *device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);

  memset (r, 0, sizeof *r);
  fake_init (&r->fake, (const FakeStep *) s->steps->data, s->steps->len);
  r->fake.hook_data = r;
  r->acquire = sidetouch_acquire_start (device, &transport, seq, mode, 1000, &callbacks, r);
  while (!r->done || r->fake.pending)
    g_main_context_iteration (NULL, TRUE);
  if (r->fake.mismatch)
    g_error ("script mismatch: %s", r->fake.mismatch);
  g_assert_cmpuint (r->fake.index, ==, r->fake.n_steps);
  fake_clear (&r->fake);
  g_object_unref (device);
}

/* Marks the last scripted step to cancel the acquisition before completing. */
static void
cancel_at_last (Script *s)
{
  g_array_index (s->steps, FakeStep, s->steps->len - 1).before = cancel_hook;
}

/* ---- tests -------------------------------------------------------------- */

static void
test_touch (void)
{
  Script s;
  Run r;
  guint16 seq = 10, expect;

  script_init (&s);
  expect = setup (&s, 10, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 2);
  expect = readout (&s, expect, 0x001e, 0x005e);
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_no_error (r.error);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_true (r.samples_equal);
  g_assert_true (r.settings_equal);
  g_assert_cmpuint (r.status, ==, 0x001e);
  g_assert_cmpuint (r.armed, ==, 1);
  g_assert_cmpuint (r.contact, ==, 1);
  g_assert_cmpuint (seq, ==, expect);
  g_assert_cmpuint (seq, ==, 10 + 4 + 3 + SIDETOUCH_ACQUISITION_CHUNKS);
  script_clear (&s);
}

static void
test_untouched (void)
{
  Script s;
  Run r;
  guint16 seq = 3, expect;

  script_init (&s);
  expect = setup (&s, 3, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 1);
  expect = readout (&s, expect, 0x005a, 0x001a);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_no_error (r.error);
  g_assert_true (r.samples_equal);
  g_assert_false (r.have_settings);
  g_assert_cmpuint (r.status, ==, 0x005a);
  g_assert_cmpuint (r.armed + r.contact, ==, 0);
  g_assert_cmpuint (seq, ==, expect);
  script_clear (&s);
}

/* Touch mode: a status word the mode refuses fails past the boundary, with
 * the reader state unknown. */
static void
check_touch_status_refused (guint16 status, guint16 ready)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 0);
  serial_read (&s, drain, sizeof drain);
  EXCHANGE (&s, next++, cmd_status, 0x00, (guint8) status, (guint8) (status >> 8), 0x00);
  if (sidetouch_touch_status_ok (status))
    {
      EXCHANGE (&s, next++, cmd_clear, 0x00);
      EXCHANGE (&s, next++, cmd_ready, 0x00, (guint8) ready, (guint8) (ready >> 8));
    }
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);
}

/* Untouched mode: the flag register is cleared after every well-formed 01 43
 * reply; a frame the reader refuses ends the acquisition with the reader state
 * known, and no samples. at_ready: the refusal comes from 01 49. */
static void
check_untouched_refused (guint16 status, guint8 error_byte, gboolean at_ready, guint16 ready)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 0);
  serial_read (&s, drain, sizeof drain);
  EXCHANGE (&s, next++, cmd_status, 0x00, (guint8) status, (guint8) (status >> 8), error_byte);
  EXCHANGE (&s, next++, cmd_clear, 0x00);
  if (at_ready)
    EXCHANGE (&s, next++, cmd_ready, 0x00, (guint8) ready, (guint8) (ready >> 8));
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, SIDETOUCH_ACQUIRE_ERROR, SIDETOUCH_ACQUIRE_ERROR_REFUSED);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_false (r.have_samples);
  g_assert_cmpuint (seq, ==, next);
  g_clear_error (&r.error);
  script_clear (&s);
}

/* Untouched mode: the contact correlate (bit 0x04) does not stop the
 * acquisition; the frame is read out and the word reported for the guards. */
static void
check_untouched_read_out (guint16 status, guint16 ready)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 0);
  next = readout (&s, next, status, ready);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_no_error (r.error);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_true (r.samples_equal);
  g_assert_cmpuint (r.status, ==, status);
  g_assert_cmpuint (seq, ==, next);
  script_clear (&s);
}

/* Untouched mode: a malformed 01 43 reply, or a failed clear, is still a
 * protocol failure past the boundary. */
static void
test_untouched_protocol_failures (void)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  /* a non-zero command status in the 01 43 reply */
  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 0);
  serial_read (&s, drain, sizeof drain);
  EXCHANGE (&s, next++, cmd_status, 0x01, 0x5a, 0x00, 0x00);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);

  /* the clear after a refused word fails */
  seq = 1;
  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 0);
  serial_read (&s, drain, sizeof drain);
  EXCHANGE (&s, next++, cmd_status, 0x00, 0x12, 0x00, 0x00);
  EXCHANGE (&s, next++, cmd_clear, 0x01);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);
}

static void
test_status_policy (void)
{
  check_touch_status_refused (0x001a, 0x001e);               /* no contact bit */
  check_touch_status_refused (0x001e, 0x001a);               /* 01 49 refused */
  check_untouched_refused (0x0012, 0x00, FALSE, 0);          /* bit 0x08 missing */
  check_untouched_refused (0x000a, 0x00, FALSE, 0);          /* bit 0x10 missing */
  check_untouched_refused (0x001a, 0x13, FALSE, 0);          /* device error byte */
  check_untouched_refused (0x005a, 0x00, TRUE, 0x000a);      /* 01 49 without bit 0x10 */
  check_untouched_read_out (0x001e, 0x001e);                 /* contact correlate set: read out */
  check_untouched_read_out (0x005a, 0x0010);                 /* 01 49 needs bit 0x10 only */
}

static guint16
cancel_sequence (Script *s, guint16 seq, const guint8 *cancelled_status)
{
  guint8 frame[SIDETOUCH_USB_PACKET];
  gsize n = sidetouch_eagle_request (seq++, NULL, 0, frame, sizeof frame);

  out (s, frame, n);
  IN (s, ack);
  step (s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 60 });
  OUT (s, poll); IN (s, st_d8);
  serial_read (s, drain, sizeof drain);
  exchange (s, seq++, cmd_status, sizeof cmd_status, cancelled_status, 4);
  EXCHANGE (s, seq++, cmd_clear, 0x00);
  return seq;
}

static void
test_cancel_during_wait (void)
{
  static const guint8 cancelled[] = { 0x00, 0x4a, 0x00, 0x13 };
  Script s;
  Run r;
  guint16 seq = 20, next;

  script_init (&s);
  next = setup (&s, 20, SIDETOUCH_MODE_TOUCH);
  OUT (&s, poll); IN (&s, st_wait);
  step (&s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50 });
  cancel_at_last (&s);
  next = cancel_sequence (&s, next, cancelled);
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_cmpuint (r.armed, ==, 1);
  g_assert_cmpuint (r.contact, ==, 0);
  g_assert_cmpuint (seq, ==, next);   /* the empty frame took a sequence number */
  g_clear_error (&r.error);
  script_clear (&s);
}

static void
test_cancel_sequence_failure (void)
{
  static const guint8 wrong[] = { 0x00, 0x1e, 0x00, 0x00 };
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  OUT (&s, poll);
  step (&s, (FakeStep) { .kind = FAKE_RECEIVE, .data = st_wait, .length = sizeof st_wait, .before = cancel_hook });
  {
    guint8 frame[SIDETOUCH_USB_PACKET];
    gsize n = sidetouch_eagle_request (next++, NULL, 0, frame, sizeof frame);

    out (&s, frame, n);
    IN (&s, ack);
    step (&s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 60 });
    OUT (&s, poll); IN (&s, st_ready);
    serial_read (&s, drain, sizeof drain);
    exchange (&s, next++, cmd_status, sizeof cmd_status, wrong, sizeof wrong);
  }
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);
}

static void
test_cancel_before_setup (void)
{
  Script s;
  Run r;
  guint16 seq = 1;

  script_init (&s);
  OUT (&s, poll);
  step (&s, (FakeStep) { .kind = FAKE_RECEIVE, .data = st_wait, .length = sizeof st_wait, .before = cancel_hook });
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_cmpuint (r.armed, ==, 0);
  g_clear_error (&r.error);
  script_clear (&s);
}

static void
test_cancel_after_contact_is_deferred (void)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 0);
  {
    guint first = s.steps->len;   /* the drain's length request, just past the boundary */

    readout (&s, next, 0x005e, 0x001e);
    g_array_index (s.steps, FakeStep, first).before = cancel_hook;
  }
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_no_error (r.error);
  g_assert_true (r.samples_equal);
  script_clear (&s);

  /* untouched mode cannot be interrupted at all */
  script_init (&s);
  seq = 1;
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  g_array_index (s.steps, FakeStep, 3).before = cancel_hook;
  wait_polls (&s, 1);
  readout (&s, next, 0x001a, 0x001a);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_no_error (r.error);
  g_assert_true (r.samples_equal);
  script_clear (&s);
}

/* A wait still pending at the deadline (a finger resting on the sensor holds
 * mode 3) is stopped through the cancellation sequence and refused, with the
 * reader state known: found before the delay, or before the next poll. */
static void
test_untouched_deadline (void)
{
  /* the untouched-mode form the reader gave in the live RA1 rerun */
  static const guint8 cancelled[] = { 0x00, 0x02, 0x00, 0x13 };
  Script s;
  Run r;
  guint16 seq = 1, next;

  for (int where = 0; where < 2; where++)
    {
      script_init (&s);
      seq = 1;
      next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
      OUT (&s, poll);
      if (where == 0)
        step (&s, (FakeStep) { .kind = FAKE_RECEIVE, .data = st_wait, .length = sizeof st_wait,
                               .sleep_ms = SIDETOUCH_UNTOUCHED_DEADLINE_MS + 100 });
      else
        {
          IN (&s, st_wait);
          step (&s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50, .sleep_ms = SIDETOUCH_UNTOUCHED_DEADLINE_MS + 100 });
        }
      next = cancel_sequence (&s, next, cancelled);
      run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
      g_assert_error (r.error, SIDETOUCH_ACQUIRE_ERROR, SIDETOUCH_ACQUIRE_ERROR_REFUSED);
      g_assert_nonnull (strstr (r.error->message, "still pending"));
      g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
      g_assert_false (r.have_samples);
      g_assert_cmpuint (seq, ==, next);
      g_clear_error (&r.error);
      script_clear (&s);
    }

  /* a cancellation the reader does not confirm leaves the state unknown */
  {
    static const guint8 wrong[] = { 0x00, 0x02, 0x00, 0x00 };

    script_init (&s);
    seq = 1;
    next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
    OUT (&s, poll);
    step (&s, (FakeStep) { .kind = FAKE_RECEIVE, .data = st_wait, .length = sizeof st_wait,
                           .sleep_ms = SIDETOUCH_UNTOUCHED_DEADLINE_MS + 100 });
    {
      guint8 frame[SIDETOUCH_USB_PACKET];
      gsize n = sidetouch_eagle_request (next++, NULL, 0, frame, sizeof frame);

      out (&s, frame, n);
      IN (&s, ack);
      step (&s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 60 });
      OUT (&s, poll); IN (&s, st_d8);
      serial_read (&s, drain, sizeof drain);
      exchange (&s, next++, cmd_status, sizeof cmd_status, wrong, sizeof wrong);
    }
    run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
    g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
    g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
    g_clear_error (&r.error);
    script_clear (&s);
  }
}

/* The deadline holds inside an exchange and for a success: a slow final
 * range read is not reported as an acquisition, and a deadline passing mid
 * exchange stops the transfer in flight. */
static void
test_untouched_deadline_inside_exchanges (void)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 1);
  readout (&s, next, 0x005a, 0x001a);
  g_array_index (s.steps, FakeStep, s.steps->len - 1).sleep_ms = SIDETOUCH_UNTOUCHED_DEADLINE_MS + 100;
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_assert_false (r.have_samples);
  g_clear_error (&r.error);
  script_clear (&s);

  script_init (&s);
  seq = 1;
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  wait_polls (&s, 1);
  {
    /* past the drain (4 steps) and 01 43, 02 43, 01 49 (12 steps each): the
     * first range read's first preflight reply */
    guint at = s.steps->len + 4 + 3 * 12 + 1;

    readout (&s, next, 0x005a, 0x001a);
    g_array_index (s.steps, FakeStep, at).sleep_ms = SIDETOUCH_UNTOUCHED_DEADLINE_MS + 100;
    /* the next transfer is stopped by the deadline; nothing after it runs */
    g_array_set_size (s.steps, at + 2);
  }
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_assert_nonnull (strstr (r.error->message, "deadline"));
  g_clear_error (&r.error);
  script_clear (&s);
}

/* Only bit 0x20 of the wait state is read: set pending, clear ready. A ready
 * state other than 0xda (the reader once sent 0x04 in an untouched build)
 * goes on to the drain, and the drain still decides. */
static void
test_wait_state_bit (void)
{
  static const guint8 st_04[] = { 0x32, 0x60, 0x04 };
  Script s;
  Run r;
  guint16 seq = 1, next;

  /* 0x04, then the ready drain: read out as usual */
  script_init (&s);
  next = setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  OUT (&s, poll); IN (&s, st_wait);
  step (&s, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = 50 });
  OUT (&s, poll); IN (&s, st_04);
  next = readout (&s, next, 0x005a, 0x001a);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_no_error (r.error);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_KNOWN);
  g_assert_true (r.samples_equal);
  g_assert_cmpuint (seq, ==, next);
  script_clear (&s);

  /* 0x04 not followed by the ready drain: a protocol failure, reader unknown */
  script_init (&s);
  seq = 1;
  setup (&s, 1, SIDETOUCH_MODE_UNTOUCHED);
  OUT (&s, poll); IN (&s, st_04);
  serial_read (&s, drain_bad, sizeof drain_bad);
  run (&r, &s, SIDETOUCH_MODE_UNTOUCHED, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);
}

static void
test_bad_drain_chunk_and_stream (void)
{
  Script s;
  Run r;
  guint16 seq = 1, next;

  /* a ready drain that differs */
  script_init (&s);
  setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 0);
  serial_read (&s, drain_bad, sizeof drain_bad);
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_assert_cmpuint (r.contact, ==, 0);
  g_clear_error (&r.error);
  script_clear (&s);

  /* a range read with a non-zero status byte */
  script_init (&s);
  seq = 1;
  next = setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 0);
  serial_read (&s, drain, sizeof drain);
  EXCHANGE (&s, next++, cmd_status, 0x00, 0x1e, 0x00, 0x00);
  EXCHANGE (&s, next++, cmd_clear, 0x00);
  EXCHANGE (&s, next++, cmd_ready, 0x00, 0x1e, 0x00);
  {
    guint8 cmd[5], payload[1 + SIDETOUCH_CHUNK_LENGTH];

    sidetouch_cmd_range (0, SIDETOUCH_CHUNK_LENGTH, cmd, sizeof cmd);
    memset (payload, 0, sizeof payload);
    payload[0] = 1;
    exchange (&s, next++, cmd, sizeof cmd, payload, sizeof payload);
  }
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);

  /* a stream that does not decode */
  stream[0] ^= 1;
  script_init (&s);
  seq = 1;
  next = setup (&s, 1, SIDETOUCH_MODE_TOUCH);
  wait_polls (&s, 0);
  readout (&s, next, 0x001e, 0x001e);
  run (&r, &s, SIDETOUCH_MODE_TOUCH, &seq);
  g_assert_error (r.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (r.reader, ==, SIDETOUCH_READER_UNKNOWN);
  g_clear_error (&r.error);
  script_clear (&s);
  stream[0] ^= 1;
}

int
main (int argc, char **argv)
{
  make_fixtures ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sidetouch/acquire/touch", test_touch);
  g_test_add_func ("/sidetouch/acquire/untouched", test_untouched);
  g_test_add_func ("/sidetouch/acquire/status-policy", test_status_policy);
  g_test_add_func ("/sidetouch/acquire/untouched-protocol-failures", test_untouched_protocol_failures);
  g_test_add_func ("/sidetouch/acquire/cancel-during-wait", test_cancel_during_wait);
  g_test_add_func ("/sidetouch/acquire/cancel-sequence-failure", test_cancel_sequence_failure);
  g_test_add_func ("/sidetouch/acquire/cancel-before-setup", test_cancel_before_setup);
  g_test_add_func ("/sidetouch/acquire/cancel-after-contact-is-deferred", test_cancel_after_contact_is_deferred);
  g_test_add_func ("/sidetouch/acquire/untouched-deadline", test_untouched_deadline);
  g_test_add_func ("/sidetouch/acquire/untouched-deadline-inside-exchanges", test_untouched_deadline_inside_exchanges);
  g_test_add_func ("/sidetouch/acquire/wait-state-bit", test_wait_state_bit);
  g_test_add_func ("/sidetouch/acquire/bad-drain-chunk-and-stream", test_bad_drain_chunk_and_stream);
  return g_test_run ();
}
