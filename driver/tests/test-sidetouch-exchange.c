/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-exchange over the scripted transport, with recorded
 * non-biometric frames: the firmware query (sequence 1, firmware 5.9). */
#include "drivers_api.h"
#include "sidetouch-exchange.h"
#include "sidetouch-fake.h"
#include "sidetouch-proto.h"
#include "test-device-fake.h"

static const guint8 fw_request[] = {
  0x82, 0x40, 0x42, 0x31, 0x12, 0x00, 0x00, 0x04, 0x00, 0x10, 0x00, 0x2f, 0x00, 0x0b, 0x00,
  0xba, 0xab, 0x01, 0x00, 0x0a, 0x00, 0xc4, 0xba, 0x01, 0x42, 0x82, 0x42, 0x42,
};
static const guint8 fw_command[] = { 0x01, 0x42 };
static const guint8 fw_response[] = {
  0x32, 0x60, 0x62, 0x1b, 0x00, 0x1f, 0x00, 0x0b, 0x00,
  0xca, 0xf0, 0x01, 0x00, 0x15, 0x00, 0xc1, 0xd1,
  0x00, 0x05, 0x09, 0x68, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
};
static const guint8 poll[] = { 0x81 };
static const guint8 pending[] = { 0x32, 0x60 };
static const guint8 st_wait[] = { 0x32, 0x60, 0xfa };
static const guint8 st_ready[] = { 0x32, 0x60, 0xda };
static const guint8 ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62 };
static const guint8 length_request[] = { 0x82, 0x40, 0x42, 0x31, 0x02, 0x00, 0x01, 0xff, 0xff, 0x82, 0x42, 0x42 };
static const guint8 fw_length[] = { 0x32, 0x60, 0x62, 0x1b, 0x00 };
static guint8 fw_data_request[27 + 10];
static guint8 drain_data_request[4 + 10];
static const guint8 drain_length[] = { 0x32, 0x60, 0x62, 0x04, 0x00 };
static const guint8 drain[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x07, 0x01 };
static const guint8 empty[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x1f, 0x00 };

typedef struct
{
  gboolean    done;
  GError     *error;
  GByteArray *data;
} Result;

static void
on_done (FpDevice *device, const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  Result *r = user_data;

  (void) device;
  g_assert_false (r->done);
  r->done = TRUE;
  r->error = error;
  if (data)
    r->data = g_byte_array_append (g_byte_array_new (), data, (guint) length);
}

typedef struct
{
  Fake     fake;
  Result   result;
  FpDevice *device;
} Run;

static void
run (Run *t, const FakeStep *steps, gsize n_steps, gboolean bare, guint16 sequence,
     const guint8 *command, gsize command_length, guint timeout_ms, GCancellable *cancellable)
{
  SidetouchTransport transport = { &fake_ops, &t->fake };

  memset (&t->result, 0, sizeof t->result);
  fake_init (&t->fake, steps, n_steps);
  t->device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  if (bare)
    sidetouch_serial_read_async (t->device, &transport, timeout_ms, cancellable, on_done, &t->result);
  else
    sidetouch_exchange_async (t->device, &transport, sequence, command, command_length, timeout_ms,
                              cancellable, on_done, &t->result);
  while (!t->result.done || t->fake.pending)
    g_main_context_iteration (NULL, TRUE);
}

static void
run_clear (Run *t)
{
  g_clear_error (&t->result.error);
  if (t->result.data)
    g_byte_array_unref (t->result.data);
  g_clear_object (&t->device);
  fake_clear (&t->fake);
}

static void
assert_script_done (Run *t)
{
  if (t->fake.mismatch)
    g_error ("script mismatch: %s", t->fake.mismatch);
  g_assert_cmpuint (t->fake.index, ==, t->fake.n_steps);
}

#define PREFLIGHT FAKE_OUT (poll), FAKE_IN (st_wait), FAKE_OUT (poll), FAKE_IN (st_wait)

static void
test_firmware_query (void)
{
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (st_ready),
    FAKE_OUT (length_request), FAKE_IN (fw_length),
    FAKE_OUT (fw_data_request), FAKE_IN (fw_response),
  };
  Run t;

  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_no_error (t.result.error);
  assert_script_done (&t);
  g_assert_cmpmem (t.result.data->data, t.result.data->len, fw_response + 17, sizeof fw_response - 17);
  run_clear (&t);
}

static void
test_fragmented_and_pending (void)
{
  static const guint8 length_a[] = { 0x32, 0x60, 0x62, 0x1b };
  static const guint8 length_b[] = { 0x33, 0x61, 0x00 };   /* its own transport pair, dropped */
  static guint8 response_b[2 + sizeof fw_response - 9];
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (pending), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (pending), FAKE_IN (pending), FAKE_IN (pending), FAKE_IN (st_ready),
    FAKE_OUT (length_request), FAKE_IN (pending), FAKE_IN (length_a), FAKE_IN (length_b),
    FAKE_OUT (fw_data_request),
    { .kind = FAKE_RECEIVE, .data = fw_response, .length = 9 }, FAKE_IN (pending), FAKE_IN (response_b),
  };
  Run t;

  response_b[0] = 0x32;
  response_b[1] = 0x60;
  memcpy (response_b + 2, fw_response + 9, sizeof fw_response - 9);
  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_no_error (t.result.error);
  assert_script_done (&t);
  g_assert_cmpmem (t.result.data->data, t.result.data->len, fw_response + 17, sizeof fw_response - 17);
  run_clear (&t);
}

static void
test_poll_bound (void)
{
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (pending), FAKE_IN (pending), FAKE_IN (pending), FAKE_IN (pending),
  };
  Run t;

  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  assert_script_done (&t);
  run_clear (&t);
}

static void
run_response (Run *t, const guint8 *response, gsize length, guint16 sequence)
{
  static FakeStep steps[12];
  const FakeStep proto[] = {
    PREFLIGHT,
    FAKE_OUT_ANY, FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (st_ready),
    FAKE_OUT (length_request), FAKE_IN (fw_length),
    FAKE_OUT (fw_data_request),
  };

  G_STATIC_ASSERT (G_N_ELEMENTS (proto) + 1 == G_N_ELEMENTS (steps));
  memcpy (steps, proto, sizeof proto);
  steps[G_N_ELEMENTS (proto)] = (FakeStep) { .kind = FAKE_RECEIVE, .data = response, .length = length };
  run (t, steps, G_N_ELEMENTS (steps), FALSE, sequence, fw_command, sizeof fw_command, 1000, NULL);
}

static void
test_invalid_responses (void)
{
  guint8 bad[sizeof fw_response];
  Run t;

  /* a CRC error */
  memcpy (bad, fw_response, sizeof bad);
  bad[sizeof bad - 1] ^= 1;
  run_response (&t, bad, sizeof bad, 1);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null (t.result.data);
  run_clear (&t);

  /* another sequence number */
  run_response (&t, fw_response, sizeof fw_response, 2);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  run_clear (&t);

  /* a wrong serial read byte */
  memcpy (bad, fw_response, sizeof bad);
  bad[2] = 0x63;
  run_response (&t, bad, sizeof bad, 1);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  run_clear (&t);

  /* one byte more than announced */
  {
    guint8 longer[sizeof fw_response + 1];

    memcpy (longer, fw_response, sizeof fw_response);
    longer[sizeof fw_response] = 0;
    run_response (&t, longer, sizeof longer, 1);
    g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    run_clear (&t);
  }
}

static void
test_empty_response (void)
{
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (st_ready),
    FAKE_OUT (length_request), FAKE_IN (drain_length),
    FAKE_OUT (drain_data_request), FAKE_IN (empty),
  };
  Run t;

  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_error (t.result.error, SIDETOUCH_EXCHANGE_ERROR, SIDETOUCH_EXCHANGE_ERROR_EMPTY_RESPONSE);
  assert_script_done (&t);
  run_clear (&t);
}

static void
test_transport_error_and_cancel (void)
{
  const FakeStep failing[] = {
    PREFLIGHT,
    { .kind = FAKE_SEND, .data = fw_request, .length = sizeof fw_request, .error = G_IO_ERROR_BROKEN_PIPE },
  };
  const FakeStep cancelled[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), { .kind = FAKE_RECEIVE, .data = ack, .length = sizeof ack, .cancel = TRUE },
  };
  GCancellable *cancellable = g_cancellable_new ();
  Run t;

  run (&t, failing, G_N_ELEMENTS (failing), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_error (t.result.error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  assert_script_done (&t);
  run_clear (&t);

  run (&t, cancelled, G_N_ELEMENTS (cancelled), FALSE, 1, fw_command, sizeof fw_command, 1000, cancellable);
  g_assert_error (t.result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  assert_script_done (&t);   /* nothing is started after the cancellation */
  run_clear (&t);
  g_object_unref (cancellable);
}

static void
test_announced_length_bound (void)
{
  static const guint8 too_long[] = { 0x32, 0x60, 0x62, 0xf7, 0x01 };   /* 503 */
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (st_ready),
    FAKE_OUT (length_request), FAKE_IN (too_long),
  };
  Run t;

  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 1000, NULL);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  assert_script_done (&t);   /* no data request is sent */
  run_clear (&t);
}

static void
test_read_deadline (void)
{
  const FakeStep steps[] = {
    PREFLIGHT,
    FAKE_OUT (fw_request), FAKE_IN (ack),
    FAKE_OUT (poll), FAKE_IN (st_ready),
    FAKE_OUT (length_request),
    { .kind = FAKE_RECEIVE, .data = pending, .length = sizeof pending, .sleep_ms = 40 },
    { .kind = FAKE_RECEIVE, .data = pending, .length = sizeof pending, .sleep_ms = 40 },
  };
  Run t;

  /* two transport-only packets take longer than the read's one deadline */
  run (&t, steps, G_N_ELEMENTS (steps), FALSE, 1, fw_command, sizeof fw_command, 60, NULL);
  g_assert_error (t.result.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  assert_script_done (&t);
  run_clear (&t);
}

static void
test_serial_read (void)
{
  const FakeStep steps[] = {
    FAKE_OUT (length_request), FAKE_IN (drain_length),
    FAKE_OUT (drain_data_request), FAKE_IN (drain),
  };
  Run t;

  run (&t, steps, G_N_ELEMENTS (steps), TRUE, 0, NULL, 0, 1000, NULL);
  g_assert_no_error (t.result.error);
  assert_script_done (&t);
  g_assert_cmpmem (t.result.data->data, t.result.data->len, drain, sizeof drain);
  run_clear (&t);
}

static void
test_request_too_large (void)
{
  static guint8 big[SIDETOUCH_USB_PACKET];
  Run t;

  run (&t, NULL, 0, FALSE, 1, big, sizeof big, 1000, NULL);
  g_assert_error (t.result.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_cmpuint (t.fake.index, ==, 0);
  run_clear (&t);
}

int
main (int argc, char **argv)
{
  g_assert_cmpuint (sidetouch_serial_data_request (27, fw_data_request, sizeof fw_data_request), ==,
                    sizeof fw_data_request);
  g_assert_cmpuint (sidetouch_serial_data_request (4, drain_data_request, sizeof drain_data_request), ==,
                    sizeof drain_data_request);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sidetouch/exchange/firmware-query", test_firmware_query);
  g_test_add_func ("/sidetouch/exchange/fragmented-and-pending", test_fragmented_and_pending);
  g_test_add_func ("/sidetouch/exchange/poll-bound", test_poll_bound);
  g_test_add_func ("/sidetouch/exchange/invalid-responses", test_invalid_responses);
  g_test_add_func ("/sidetouch/exchange/empty-response", test_empty_response);
  g_test_add_func ("/sidetouch/exchange/transport-error-and-cancel", test_transport_error_and_cancel);
  g_test_add_func ("/sidetouch/exchange/announced-length-bound", test_announced_length_bound);
  g_test_add_func ("/sidetouch/exchange/read-deadline", test_read_deadline);
  g_test_add_func ("/sidetouch/exchange/serial-read", test_serial_read);
  g_test_add_func ("/sidetouch/exchange/request-too-large", test_request_too_large);
  return g_test_run ();
}
