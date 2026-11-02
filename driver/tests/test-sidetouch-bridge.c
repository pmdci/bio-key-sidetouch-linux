/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-bridge over the scripted transport. */
#include "drivers_api.h"
#include "sidetouch-bridge.h"
#include "sidetouch-fake.h"
#include "test-device-fake.h"

static const guint8 poll[] = { 0x81 };
static const guint8 two[] = { 0x32, 0x60 };
static const guint8 length_request[] = { 0x82, 0x40, 0x42, 0x31, 0x02, 0x00, 0x01, 0xff, 0xff, 0x82, 0x42, 0x42 };
static const guint8 setup_1[] = { 0x8a };
static const guint8 setup_2[] = { 0x86, 0x03, 0x00 };
static const guint8 setup_3[] = { 0x9e, 0x00, 0x40 };
static const guint8 setup_4[] = { 0x80, 0x00, 0x03, 0x82, 0x42, 0x42 };
static const guint8 setup_5[] = { 0x80, 0x42, 0x03, 0x82, 0x42, 0x42 };
static const guint8 seven[] = {
  0x82, 0x40, 0x42, 0x31, 0x07, 0x00, 0x00, 0x04, 0x00, 0x05, 0x00, 0x2f, 0x00, 0x00, 0x82, 0x42, 0x42,
};
static const guint8 ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62 };
static const guint8 read_5[] = {
  0x82, 0x40, 0x42, 0x31, 0x05, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0x82, 0x42, 0x42,
};
static const guint8 length_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00 };
static const guint8 final_80[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x1f, 0x00, 0x80 };
static const guint8 final_00[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x1f, 0x00, 0x00 };
static const guint8 final_bad[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x1f, 0x00, 0x81 };
static const guint8 st_idle[] = { 0x32, 0x60, 0x00 };
static const guint8 st_done[] = { 0x32, 0x60, 0x20 };
static const guint8 field_0[] = { 0x32, 0x60, 0x62, 0x00, 0x00 };
static const guint8 field_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00 };
static const guint8 field_513[] = { 0x32, 0x60, 0x62, 0x01, 0x02 };
static const guint8 field_bad[] = { 0x32, 0x60, 0x63, 0x00, 0x00 };
static const guint8 payload_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x11, 0x22, 0x33 };

static void
add (GArray *a, FakeStep step)
{
  g_array_append_val (a, step);
}

#define OUT(b) add (a, (FakeStep) FAKE_OUT (b))
#define IN(b) add (a, (FakeStep) FAKE_IN (b))
#define CTRL(r, v, i) add (a, (FakeStep) { .kind = FAKE_CONTROL, .request = (r), .value = (v), .index = (i) })
#define WAIT(ms) add (a, (FakeStep) { .kind = FAKE_WAIT, .delay_ms = (ms) })

static void
control_phase (GArray *a)
{
  CTRL (0x00, 0, 0); CTRL (0x00, 0, 1); CTRL (0x00, 0, 2); IN (two);
  CTRL (0x00, 0, 0); CTRL (0x06, 0, 0); CTRL (0x07, 0, 0); IN (two);
  CTRL (0x09, 1, 0); CTRL (0x0b, 0, 0); IN (two);
  CTRL (0x0b, 0x200, 0); IN (two);
  CTRL (0x00, 0, 0);
}

static void
preamble (GArray *a)
{
  OUT (setup_1); OUT (setup_2); CTRL (0x00, 0, 0); OUT (setup_3); OUT (setup_4); OUT (setup_5);
  OUT (poll); IN (two); IN (st_idle);
}

/* One sweep pass with bit 0x20 clear and the given field reply. */
static void
pass (GArray *a, const guint8 *field, gsize field_length)
{
  OUT (poll); IN (st_idle);
  OUT (length_request);
  add (a, (FakeStep) { .kind = FAKE_RECEIVE, .data = field, .length = field_length });
}

static void
pass_ok (GArray *a)
{
  pass (a, field_0, sizeof field_0);
  WAIT (5);
}

static void
final_phase (GArray *a, const guint8 *last, gsize last_length)
{
  OUT (seven); IN (ack);
  OUT (poll); IN (st_idle);
  OUT (length_request); IN (length_5);
  OUT (read_5);
  add (a, (FakeStep) { .kind = FAKE_RECEIVE, .data = last, .length = last_length });
}

static void
sweep_done (GArray *a)
{
  OUT (poll); IN (st_done);
}

static void
recovery_full (GArray *a)
{
  add (a, (FakeStep) { .kind = FAKE_RECOVER });
  WAIT (200);
  OUT (poll); IN (two); IN (st_idle);
  WAIT (100);
}

typedef struct
{
  gboolean done;
  GError  *error;
} Result;

static void
on_done (FpDevice *device, GError *error, gpointer user_data)
{
  Result *r = user_data;

  (void) device;
  g_assert_false (r->done);
  r->done = TRUE;
  r->error = error;
}

static GError *
run (GArray *a, GCancellable *cancellable, gboolean expect_script_done)
{
  Fake fake;
  Result r = { 0 };
  SidetouchTransport transport = { &fake_ops, &fake };
  FpDevice *device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);

  fake_init (&fake, (const FakeStep *) a->data, a->len);
  sidetouch_bridge_init_async (device, &transport, 1000, cancellable, on_done, &r);
  while (!r.done || fake.pending)
    g_main_context_iteration (NULL, TRUE);
  if (fake.mismatch)
    g_error ("script mismatch: %s", fake.mismatch);
  if (expect_script_done)
    g_assert_cmpuint (fake.index, ==, a->len);
  fake_clear (&fake);
  g_object_unref (device);
  g_array_unref (a);
  return r.error;
}

static GArray *
script (void)
{
  return g_array_new (FALSE, TRUE, sizeof (FakeStep));
}

static void
test_happy_path (void)
{
  GArray *a = script ();

  control_phase (a);
  preamble (a);
  pass_ok (a);
  /* a pass whose field asks for a five-byte payload */
  pass (a, field_5, sizeof field_5);
  OUT (read_5); IN (payload_5);
  WAIT (5);
  sweep_done (a);
  final_phase (a, final_80, sizeof final_80);
  g_assert_no_error (run (a, NULL, TRUE));

  /* the other observed final byte */
  a = script ();
  control_phase (a);
  preamble (a);
  sweep_done (a);
  final_phase (a, final_00, sizeof final_00);
  g_assert_no_error (run (a, NULL, TRUE));
}

static void
test_pass_bound (void)
{
  GArray *a = script ();

  control_phase (a);
  preamble (a);
  /* bit 0x20 never set: the 20th status ends the sweep as complete */
  for (int i = 0; i < 19; i++)
    pass_ok (a);
  OUT (poll); IN (st_idle);
  final_phase (a, final_80, sizeof final_80);
  g_assert_no_error (run (a, NULL, TRUE));
}

static void
test_recovery_then_success (void)
{
  GArray *a = script ();

  control_phase (a);
  preamble (a);
  pass (a, field_bad, sizeof field_bad);
  recovery_full (a);
  sweep_done (a);
  final_phase (a, final_80, sizeof final_80);
  g_assert_no_error (run (a, NULL, TRUE));

  /* a failed IN recovery goes straight to the 100 ms gap */
  a = script ();
  control_phase (a);
  preamble (a);
  pass (a, field_513, sizeof field_513);
  add (a, (FakeStep) { .kind = FAKE_RECOVER, .error = G_IO_ERROR_BROKEN_PIPE });
  WAIT (100);
  sweep_done (a);
  final_phase (a, final_80, sizeof final_80);
  g_assert_no_error (run (a, NULL, TRUE));
}

static void
test_third_sweep_is_terminal (void)
{
  GArray *a = script ();
  GError *error;

  control_phase (a);
  preamble (a);
  pass (a, field_bad, sizeof field_bad);
  recovery_full (a);
  pass (a, field_bad, sizeof field_bad);
  WAIT (100);
  sweep_done (a);   /* the third sweep completes, and still fails */
  error = run (a, NULL, TRUE);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_clear_error (&error);

  /* three failed sweeps */
  a = script ();
  control_phase (a);
  preamble (a);
  pass (a, field_bad, sizeof field_bad);
  recovery_full (a);
  pass (a, field_bad, sizeof field_bad);
  WAIT (100);
  pass (a, field_bad, sizeof field_bad);
  error = run (a, NULL, TRUE);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_clear_error (&error);
}

static void
test_final_mismatch_and_short_drain (void)
{
  static const guint8 one[] = { 0x32 };
  GArray *a = script ();
  GError *error;

  control_phase (a);
  preamble (a);
  sweep_done (a);
  final_phase (a, final_bad, sizeof final_bad);
  error = run (a, NULL, TRUE);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_clear_error (&error);

  /* an exact-match reply that differs */
  {
    static const guint8 length_6[] = { 0x32, 0x60, 0x62, 0x06, 0x00 };

    a = script ();
    control_phase (a);
    preamble (a);
    sweep_done (a);
    OUT (seven); IN (ack);
    OUT (poll); IN (st_idle);
    OUT (length_request); IN (length_6);
    error = run (a, NULL, TRUE);
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    g_clear_error (&error);
  }

  /* a drain shorter than the transport pair is fatal, with no recovery */
  a = script ();
  CTRL (0x00, 0, 0); CTRL (0x00, 0, 1); CTRL (0x00, 0, 2); IN (one);
  error = run (a, NULL, TRUE);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_clear_error (&error);
}

static void
test_cancel (void)
{
  GCancellable *c = g_cancellable_new ();
  GArray *a = script ();
  GError *error;

  CTRL (0x00, 0, 0);
  add (a, (FakeStep) { .kind = FAKE_CONTROL, .request = 0x00, .value = 0, .index = 1, .cancel = TRUE });
  error = run (a, c, TRUE);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_object_unref (c);

  /* a cancellation during the sweep ends it, without recovery */
  c = g_cancellable_new ();
  a = script ();
  control_phase (a);
  preamble (a);
  OUT (poll);
  add (a, (FakeStep) { .kind = FAKE_RECEIVE, .data = st_idle, .length = sizeof st_idle, .cancel = TRUE });
  error = run (a, c, TRUE);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_object_unref (c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sidetouch/bridge/happy-path", test_happy_path);
  g_test_add_func ("/sidetouch/bridge/pass-bound", test_pass_bound);
  g_test_add_func ("/sidetouch/bridge/recovery-then-success", test_recovery_then_success);
  g_test_add_func ("/sidetouch/bridge/third-sweep-is-terminal", test_third_sweep_is_terminal);
  g_test_add_func ("/sidetouch/bridge/final-mismatch-and-short-drain", test_final_mismatch_and_short_drain);
  g_test_add_func ("/sidetouch/bridge/cancel", test_cancel);
  return g_test_run ();
}
