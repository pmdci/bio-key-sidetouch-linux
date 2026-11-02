/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-open over the scripted transport. */
#include "drivers_api.h"
#include "sidetouch-fake.h"
#include "sidetouch-open.h"
#include "sidetouch-proto.h"
#include "sidetouch-script.h"
#include "test-device-fake.h"

static const guint8 cmd_fw[] = { 0x01, 0x42 };
static const guint8 empty[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x1f, 0x00 };

typedef struct
{
  gboolean claim_fails;
  guint    claims;
  guint    releases;
} Interface;

static gboolean
claim (gpointer backend, GError **error)
{
  Interface *i = backend;

  i->claims++;
  if (i->claim_fails)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "busy");
      return FALSE;
    }
  return TRUE;
}

static void
release (gpointer backend)
{
  ((Interface *) backend)->releases++;
}

static const SidetouchInterfaceOps interface_ops = { claim, release, NULL };

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
run (Script *s, Interface *iface, guint16 *seq)
{
  Fake fake;
  Result r = { 0 };
  SidetouchTransport transport = { &fake_ops, &fake };
  FpDevice *device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);

  fake_init (&fake, (const FakeStep *) s->steps->data, s->steps->len);
  sidetouch_open_async (device, &transport, &interface_ops, iface, seq, 1000, NULL, on_done, &r);
  while (!r.done || fake.pending)
    g_main_context_iteration (NULL, TRUE);
  if (fake.mismatch)
    g_error ("script mismatch: %s", fake.mismatch);
  g_assert_cmpuint (fake.index, ==, fake.n_steps);
  fake_clear (&fake);
  g_object_unref (device);
  return r.error;
}

static void
test_open (void)
{
  Script s;
  Interface iface = { 0 };
  guint16 seq = 1;

  script_init (&s);
  bridge_ok (&s);
  EXCHANGE (&s, 1, cmd_fw, 0x00, 0x05, 0x09, 0x68, 0x00);
  g_assert_no_error (run (&s, &iface, &seq));
  g_assert_cmpuint (seq, ==, 2);
  g_assert_cmpuint (iface.claims, ==, 1);
  g_assert_cmpuint (iface.releases, ==, 0);
  script_clear (&s);
}

static void
test_resync_on_empty_response (void)
{
  Script s;
  Interface iface = { 0 };
  guint16 seq = 7;
  guint8 frame[512];

  script_init (&s);
  bridge_ok (&s);
  /* sequences 7 and 8 were used before: empty responses */
  for (guint16 n = 7; n <= 8; n++)
    {
      static const guint8 poll[] = { 0x81 }, wait[] = { 0x32, 0x60, 0xfa }, ready[] = { 0x32, 0x60, 0xda },
                          ack[] = { 0x32, 0x60, 0x62, 0x62 };
      gsize len = sidetouch_eagle_request (n, cmd_fw, sizeof cmd_fw, frame, sizeof frame);

      OUT (&s, poll); IN (&s, wait); OUT (&s, poll); IN (&s, wait);
      out (&s, frame, len);
      IN (&s, ack);
      OUT (&s, poll); IN (&s, ready);
      serial_read (&s, empty, sizeof empty);
    }
  EXCHANGE (&s, 9, cmd_fw, 0x00, 0x05, 0x09);
  g_assert_no_error (run (&s, &iface, &seq));
  g_assert_cmpuint (seq, ==, 10);
  g_assert_cmpuint (iface.releases, ==, 0);
  script_clear (&s);
}

/* An untested firmware version opens, with a warning naming it. (5.9 opens
 * without one: g_test_init makes any unexpected warning fatal in test_open.) */
static void
test_untested_firmware (void)
{
  static const guint8 versions[][2] = { { 6, 0 }, { 5, 10 } };

  for (gsize i = 0; i < G_N_ELEMENTS (versions); i++)
    {
      Script s;
      Interface iface = { 0 };
      guint16 seq = 1;
      g_autofree char *pattern = g_strdup_printf ("*firmware %u.%u is untested*", versions[i][0], versions[i][1]);

      script_init (&s);
      bridge_ok (&s);
      EXCHANGE (&s, 1, cmd_fw, 0x00, versions[i][0], versions[i][1]);
      g_test_expect_message ("libfprint-sidetouch", G_LOG_LEVEL_WARNING, pattern);
      g_assert_no_error (run (&s, &iface, &seq));
      g_test_assert_expected_messages ();
      g_assert_cmpuint (seq, ==, 2);
      g_assert_cmpuint (iface.releases, ==, 0);
      script_clear (&s);
    }
}

static void
test_failures_release (void)
{
  Script s;
  Interface iface = { 0 };
  guint16 seq = 1;
  GError *error;

  /* a failed firmware query */
  script_init (&s);
  bridge_ok (&s);
  EXCHANGE (&s, 1, cmd_fw, 0x01, 0x05, 0x09);
  error = run (&s, &iface, &seq);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_clear_error (&error);
  g_assert_cmpuint (iface.releases, ==, 1);
  script_clear (&s);

  /* a bridge failure */
  script_init (&s);
  step (&s, (FakeStep) { .kind = FAKE_CONTROL, .error = G_IO_ERROR_BROKEN_PIPE });
  error = run (&s, &iface, &seq);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_clear_error (&error);
  g_assert_cmpuint (iface.releases, ==, 2);
  script_clear (&s);

  /* a failed claim releases nothing and sends nothing */
  script_init (&s);
  iface.claim_fails = TRUE;
  error = run (&s, &iface, &seq);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error (&error);
  g_assert_cmpuint (iface.claims, ==, 3);
  g_assert_cmpuint (iface.releases, ==, 2);
  script_clear (&s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sidetouch/open/open", test_open);
  g_test_add_func ("/sidetouch/open/resync-on-empty-response", test_resync_on_empty_response);
  g_test_add_func ("/sidetouch/open/untested-firmware", test_untested_firmware);
  g_test_add_func ("/sidetouch/open/failures-release", test_failures_release);
  return g_test_run ();
}
