/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for the SideTouch FpDevice through the public libfprint API, over the
 * scripted transport, with synthetic textures (no biometric data). The driver
 * runs as a virtual-type subclass so that the core does not open USB. */
#include <math.h>

#include "drivers_api.h"
#include "sidetouch.h"
#include "sidetouch-fake.h"
#include "sidetouch-match.h"
#include "sidetouch-print.h"
#include "sidetouch-script.h"
#include "sidetouch-synth.h"

/* ---- a virtual subclass ---------------------------------------------------- */

G_DECLARE_FINAL_TYPE (TestSidetouch, test_sidetouch, TEST, SIDETOUCH, FpiDeviceSidetouch)

struct _TestSidetouch
{
  FpiDeviceSidetouch parent;
};

G_DEFINE_TYPE (TestSidetouch, test_sidetouch, FPI_TYPE_DEVICE_SIDETOUCH)

static void
test_sidetouch_init (TestSidetouch *self)
{
  (void) self;
}

static void
test_sidetouch_class_init (TestSidetouchClass *klass)
{
  FP_DEVICE_CLASS (klass)->type = FP_DEVICE_TYPE_VIRTUAL;
  FP_DEVICE_CLASS (klass)->id_table = NULL;
}

/* ---- synthetic images -------------------------------------------------------- */

static guint16 background[SIDETOUCH_PIXELS];

static void
make_background (void)
{
  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    background[k] = (guint16) (1200 + (k % SIDETOUCH_COLS) * 2 + ((k / SIDETOUCH_COLS) % 7) * 5);
}

/* background minus a finger-like texture of blobs (seed = finger), shifted */
static void
make_grid (guint16 *out, unsigned seed, double dx, double dy, double amplitude)
{
  double bx[60], by[60], bs[60], ba[60];
  unsigned x = seed * 2654435761u + 7u;

  for (int i = 0; i < 60; i++)
    {
      x = x * 1103515245u + 12345u; bx[i] = (double) ((x >> 8) % 1300) / 10.0 - 10.0;
      x = x * 1103515245u + 12345u; by[i] = (double) ((x >> 8) % 1350) / 10.0 - 10.0;
      x = x * 1103515245u + 12345u; bs[i] = 1.5 + (double) ((x >> 8) % 30) / 10.0;
      x = x * 1103515245u + 12345u; ba[i] = (double) ((x >> 8) % 2 ? 1 : -1) * (80.0 + (double) ((x >> 12) % 120));
    }
  for (int r = 0; r < SIDETOUCH_ROWS; r++)
    for (int c = 0; c < SIDETOUCH_COLS; c++)
      {
        double v = 0.0;

        for (int i = 0; i < 60; i++)
          {
            double ex = c - dx - bx[i], ey = r - dy - by[i];

            v += ba[i] * exp (-(ex * ex + ey * ey) / (2.0 * bs[i] * bs[i]));
          }
        out[r * SIDETOUCH_COLS + c] = (guint16) lround (background[r * SIDETOUCH_COLS + c] - 300.0 - amplitude * v);
      }
}

static guint8 *
stream_of (const guint16 *grid)
{
  guint8 *s = g_malloc (SIDETOUCH_FORMAT4_LENGTH);

  g_assert_cmpint (synth_encode (grid, s), ==, 0);
  return s;
}

static const double shifts[6][2] = { { 0, 0 }, { 2, -1 }, { -3, 2 }, { 1, 3 }, { -2, -2 }, { 1, -2 } };

/* touch i (0..4 enrolment, 5 probe) of a finger */
static guint8 *
touch (unsigned finger, int i)
{
  guint16 g[SIDETOUCH_PIXELS];

  make_grid (g, finger, shifts[i][0], shifts[i][1], 0.6);
  return stream_of (g);
}

static guint8 *
untouched (int bump)
{
  guint16 g[SIDETOUCH_PIXELS];

  memcpy (g, background, sizeof g);
  g[700] = (guint16) (g[700] + bump);   /* bump > 48 breaks G2 */
  return stream_of (g);
}

/* ---- the harness ----------------------------------------------------------------- */

typedef struct
{
  guint claims, releases, discards;
} Interface;

static gboolean
claim (gpointer b, GError **error)
{
  (void) error;
  ((Interface *) b)->claims++;
  return TRUE;
}

static void
release (gpointer b)
{
  ((Interface *) b)->releases++;
}

static void
discard (gpointer b)
{
  ((Interface *) b)->discards++;
}

static const SidetouchInterfaceOps interface_ops = { claim, release, discard };

typedef struct
{
  Script        s;
  Fake          fake;
  Interface     iface;
  FpDevice     *device;
  GCancellable *cancellable;
  guint16       seq;
  guint8        settings[49];
  GPtrArray    *streams;
  int           progress, retries;
} T;

static void
t_init (T *t)
{
  memset (t, 0, sizeof *t);
  script_init (&t->s);
  t->streams = g_ptr_array_new_with_free_func (g_free);
  t->seq = 1;
  script_settings (t->settings, 0);
  t->device = g_object_new (test_sidetouch_get_type (), NULL);
  t->cancellable = g_cancellable_new ();
}

static const guint8 *
keep_stream (T *t, guint8 *stream)
{
  g_ptr_array_add (t->streams, stream);
  return stream;
}

/* Builds the script so far into the fake; scripts are complete before use. */
static void
t_start (T *t)
{
  SidetouchTransport transport = { &fake_ops, &t->fake };

  fake_init (&t->fake, (const FakeStep *) t->s.steps->data, t->s.steps->len);
  t->fake.hook_data = t;
  fpi_device_sidetouch_set_transport (FPI_DEVICE_SIDETOUCH (t->device), &transport, &interface_ops, &t->iface);
}

static void
t_finish (T *t)
{
  if (fp_device_is_open (t->device))
    {
      g_autoptr(GError) error = NULL;

      g_assert_true (fp_device_close_sync (t->device, NULL, &error));
      g_assert_no_error (error);
    }
  if (t->fake.mismatch)
    g_error ("script mismatch: %s", t->fake.mismatch);
  g_assert_cmpuint (t->fake.index, ==, t->fake.n_steps);
  fake_clear (&t->fake);
  g_clear_object (&t->device);
  g_clear_object (&t->cancellable);
  g_ptr_array_unref (t->streams);
  script_clear (&t->s);
}

static void
script_enrolment (T *t, unsigned finger)
{
  for (int i = 0; i < 5; i++)
    t->seq = script_touch (&t->s, t->seq, t->settings, keep_stream (t, touch (finger, i)));
  script_pause (&t->s);
  for (int i = 0; i < 3; i++)
    t->seq = script_untouched (&t->s, t->seq, keep_stream (t, untouched (0)));
}

static void
on_progress (FpDevice *device, gint stages, FpPrint *print, gpointer user_data, GError *error)
{
  T *t = user_data;

  (void) device;
  (void) print;
  (void) stages;
  if (error)
    {
      g_assert_true (g_error_matches (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER));
      t->retries++;
    }
  else
    t->progress++;
}

static FpPrint *
enrol (T *t, GError **error)
{
  return fp_device_enroll_sync (t->device, fp_print_new (t->device), t->cancellable, on_progress, t, error);
}

/* A print made directly, without an enrolment on the script. */
static FpPrint *
make_print (T *t, unsigned finger, int settings_variant)
{
  SidetouchPrint *p = g_new0 (SidetouchPrint, 1);
  guint16 touches[5][SIDETOUCH_PIXELS];
  const uint16_t *tp[5];
  FpPrint *print = fp_print_new (t->device);
  gsize size;
  guint8 *bytes;

  for (int i = 0; i < 5; i++)
    {
      make_grid (touches[i], finger, shifts[i][0], shifts[i][1], 0.6);
      tp[i] = touches[i];
    }
  script_settings (p->settings, settings_variant);
  memcpy (p->reference, background, sizeof p->reference);
  g_assert_cmpint (sidetouch_match_enrol (tp, p->reference, &p->template, NULL), ==, SIDETOUCH_MATCH_OK);
  size = sidetouch_print_serialised_size (p);
  bytes = g_malloc (size);
  g_assert_cmpuint (sidetouch_print_serialise (p, bytes, size), ==, size);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, bytes, size, 1), NULL);
  g_free (bytes);
  sidetouch_print_clear (p);
  g_free (p);
  return print;
}

static gboolean
verify (T *t, FpPrint *print, gboolean *match, GError **error)
{
  return fp_device_verify_sync (t->device, print, t->cancellable, NULL, NULL, match, NULL, error);
}

/* ---- tests ------------------------------------------------------------------------- */

static void
test_enrol_verify_identify (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) a = NULL, b = NULL, matched = NULL;
  g_autoptr(GPtrArray) gallery = NULL;
  gboolean match = FALSE;

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  script_enrolment (&t, 1);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (1, 5)));   /* verify A: same finger */
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (2, 5)));   /* verify A: another finger */
  script_enrolment (&t, 2);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (1, 5)));   /* identify [B, A] */
  t_start (&t);

  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_no_error (error);
  a = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (a);
  g_assert_cmpint (t.progress, ==, 5);
  g_assert_cmpint (t.retries, ==, 0);

  g_assert_true (verify (&t, a, &match, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_true (verify (&t, a, &match, &error));
  g_assert_no_error (error);
  g_assert_false (match);

  b = enrol (&t, &error);
  g_assert_no_error (error);
  gallery = g_ptr_array_new ();
  g_ptr_array_add (gallery, b);
  g_ptr_array_add (gallery, a);
  g_assert_true (fp_device_identify_sync (t.device, gallery, t.cancellable, NULL, NULL, &matched, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (matched == a);

  g_assert_true (fp_device_close_sync (t.device, NULL, &error));
  g_assert_cmpuint (t.iface.claims, ==, 1);
  g_assert_cmpuint (t.iface.releases, ==, 1);
  t_finish (&t);
}

/* A failed open gives up the handle the core opened, so that the next open
 * starts afresh; a successful open and its close do not. */
static void
test_failed_open_discards (void)
{
  T t;
  g_autoptr(GError) error = NULL;

  t_init (&t);
  step (&t.s, (FakeStep) { .kind = FAKE_CONTROL, .error = G_IO_ERROR_BROKEN_PIPE });   /* the bridge's first */
  t.seq = script_open (&t.s, t.seq);
  t_start (&t);

  g_assert_false (fp_device_open_sync (t.device, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_clear_error (&error);
  g_assert_false (fp_device_is_open (t.device));
  g_assert_cmpuint (t.iface.claims, ==, 1);
  g_assert_cmpuint (t.iface.releases, ==, 1);
  g_assert_cmpuint (t.iface.discards, ==, 1);

  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_close_sync (t.device, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (t.iface.claims, ==, 2);
  g_assert_cmpuint (t.iface.releases, ==, 2);
  g_assert_cmpuint (t.iface.discards, ==, 1);
  t_finish (&t);
}

/* fprintd's enrolment: on a device that can identify, every enrolment starts
 * with an identify over all stored prints (a duplicate check), empty or not;
 * an error there other than a retry or "not found" fails the enrolment. */
static gboolean
duplicate_check (T *t, GPtrArray *all_prints, FpPrint **matched, GError **error)
{
  return fp_device_identify_sync (t->device, all_prints, t->cancellable, NULL, NULL, matched, NULL, error);
}

static void
test_fprintd_enrol_sequence (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) a = NULL, b = NULL;
  g_autoptr(GPtrArray) all = g_ptr_array_new ();
  FpPrint *matched = NULL;
  gsize before;

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  script_enrolment (&t, 1);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (2, 5)));   /* check [A]: another finger */
  script_enrolment (&t, 2);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (1, 5)));   /* check [A, B]: finger 1 again */
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_no_error (error);

  /* the first enrolment: an empty gallery gives no match, with no reader traffic */
  before = t.fake.index;
  g_assert_true (duplicate_check (&t, all, &matched, &error));
  g_assert_no_error (error);
  g_assert_null (matched);
  g_assert_cmpuint (t.fake.index, ==, before);   /* no step consumed */
  a = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (a);
  g_ptr_array_add (all, a);

  /* the second: one stored print, a touch of another finger, no match */
  g_assert_true (duplicate_check (&t, all, &matched, &error));
  g_assert_no_error (error);
  g_assert_null (matched);
  b = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (b);
  g_ptr_array_add (all, b);

  /* enrolling finger 1 again: the check finds it */
  g_assert_true (duplicate_check (&t, all, &matched, &error));
  g_assert_no_error (error);
  g_assert_true (matched == a);
  g_clear_object (&matched);
  t_finish (&t);
}

static void
test_reference_retry_and_failure (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;

  /* G2 rejects the first attempt; the second succeeds */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  for (int i = 0; i < 5; i++)
    t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (3, i)));
  script_pause (&t.s);
  t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (60)));
  t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  script_pause (&t.s);
  for (int i = 0; i < 3; i++)
    t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  print = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpint (t.retries, ==, 1);
  t_finish (&t);

  /* a finger resting through every attempt: G3 rejects all three */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  for (int i = 0; i < 5; i++)
    t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (3, i)));
  for (int attempt = 0; attempt < 3; attempt++)
    {
      script_pause (&t.s);
      for (int i = 0; i < 3; i++)
        t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, touch (3, 0)));
    }
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_clear_pointer (&print, g_object_unref);
  print = enrol (&t, &error);
  g_assert_null (print);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (t.retries, ==, 2);
  t_finish (&t);
}

/* G1: a frame carrying the contact correlate (01 43 bit 0x04) is read out,
 * and the attempt is then rejected and retried like any guard failure. A
 * frame the reader refuses ends its attempt at once, with the same retry. A
 * G1 failure on every attempt ends the enrolment cleanly, with the reader
 * still usable. */
static void
test_reference_status_guard (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL, other = NULL;
  gboolean match = FALSE;

  /* attempt 1: frame 2 carries bit 0x04 (rejected after all three frames);
   * attempt 2 accepted */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  for (int i = 0; i < 5; i++)
    t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (5, i)));
  script_pause (&t.s);
  t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  t.seq = script_untouched_status (&t.s, t.seq, keep_stream (&t, untouched (0)), 0x005e, 0x001e);
  t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  script_pause (&t.s);
  for (int i = 0; i < 3; i++)
    t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  print = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpint (t.retries, ==, 1);
  t_finish (&t);

  /* attempt 1: the reader refuses frame 1 (bit 0x08 missing); attempt 2
   * accepted */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  for (int i = 0; i < 5; i++)
    t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (5, i)));
  script_pause (&t.s);
  t.seq = script_untouched_refused (&t.s, t.seq, 0x0012);
  script_pause (&t.s);
  for (int i = 0; i < 3; i++)
    t.seq = script_untouched (&t.s, t.seq, keep_stream (&t, untouched (0)));
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_clear_pointer (&print, g_object_unref);
  print = enrol (&t, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpint (t.retries, ==, 1);
  t_finish (&t);

  /* G1 fails on every attempt: a clean enrolment failure, then a verification
   * on the same open device works (the reader state stayed known) */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  for (int i = 0; i < 5; i++)
    t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (5, i)));
  for (int attempt = 0; attempt < 3; attempt++)
    {
      script_pause (&t.s);
      for (int i = 0; i < 3; i++)
        t.seq = script_untouched_status (&t.s, t.seq, keep_stream (&t, untouched (0)), 0x001e, 0x001e);
    }
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (6, 5)));
  t_start (&t);
  other = make_print (&t, 6, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_clear_pointer (&print, g_object_unref);
  print = enrol (&t, &error);
  g_assert_null (print);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_null (strstr (error->message, "unknown"));
  g_assert_cmpint (t.retries, ==, 2);
  g_clear_error (&error);
  g_assert_true (verify (&t, other, &match, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  t_finish (&t);
}

static void
test_settings_change_during_enrolment (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  guint8 other[49];

  script_settings (other, 1);
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (8, 0)));
  t.seq = script_touch (&t.s, t.seq, other, keep_stream (&t, touch (8, 1)));
  t_start (&t);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  print = enrol (&t, &error);
  g_assert_null (print);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_assert_cmpint (t.progress, ==, 1);
  t_finish (&t);
}

static void
cancel_hook (gpointer data)
{
  g_cancellable_cancel (((T *) data)->cancellable);
}

static void
test_cancel_then_reuse (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  gboolean match = FALSE;

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t.seq = script_touch_cancelled (&t.s, t.seq, t.settings, cancel_hook, TRUE);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (4, 5)));
  t_start (&t);
  print = make_print (&t, 4, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_false (verify (&t, print, &match, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_clear_object (&t.cancellable);
  t.cancellable = g_cancellable_new ();
  g_assert_true (verify (&t, print, &match, &error));   /* the reader stayed usable */
  g_assert_no_error (error);
  g_assert_true (match);
  t_finish (&t);
}

static void
test_unknown_reader_refuses (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  gboolean match = FALSE;
  guint8 *bad = touch (5, 5);

  bad[0] ^= 1;   /* the stream does not decode: a failure past the contact boundary */
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, bad));
  t_start (&t);
  print = make_print (&t, 5, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_false (verify (&t, print, &match, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_false (verify (&t, print, &match, &error));   /* refused without a transfer */
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  t_finish (&t);
}

/* A transfer failing while waiting for a finger, as on a disconnect: the
 * action fails with the transport's error; before the contact boundary the
 * reader stays known, so the next action runs. */
static void
test_transport_failure_during_wait (void)
{
  static const guint8 poll[] = { 0x81 };
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  gboolean match = FALSE;
  guint8 cmd_info[] = { 0x01, 0x41 }, cmd_f[] = { 0x02, 0x46, 0x19, 0x00, 0x00, 0x00 },
         cmd_e[] = { 0x02, 0x45, 0x80, 0x96, 0x98, 0x00 }, cmd_touch[] = { 0x08, 0x07 };

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  exchange (&t.s, t.seq++, cmd_info, sizeof cmd_info, t.settings, 49);
  EXCHANGE (&t.s, t.seq++, cmd_f, 0x00);
  EXCHANGE (&t.s, t.seq++, cmd_e, 0x00);
  EXCHANGE (&t.s, t.seq++, cmd_touch, 0x00);
  OUT (&t.s, poll);
  step (&t.s, (FakeStep) { .kind = FAKE_RECEIVE, .error = G_IO_ERROR_BROKEN_PIPE });
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (9, 5)));
  t_start (&t);
  print = make_print (&t, 9, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_false (verify (&t, print, &match, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_clear_error (&error);
  g_assert_true (verify (&t, print, &match, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  t_finish (&t);
}

/* A print stored by another driver registered under the same id ("sidetouch")
 * is refused as invalid data, with no transfer. */
static void
test_foreign_print_refused (void)
{
  static const guint8 foreign[] = { 'P', 'R', 'I', 'V', 0x01, 0x00, 0x2a, 0x2a };
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL, empty = NULL;
  gboolean match = TRUE;

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t_start (&t);
  print = fp_print_new (t.device);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, foreign, sizeof foreign, 1),
                NULL);
  empty = fp_print_new (t.device);
  fpi_print_set_type (empty, FPI_PRINT_RAW);
  g_object_set (empty, "fpi-data", g_variant_new_string ("not bytes"), NULL);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_false (verify (&t, print, &match, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_assert_false (verify (&t, empty, &match, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  t_finish (&t);
}

static void
test_settings_and_unscorable (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) other = NULL, print = NULL;
  gboolean match = TRUE;

  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (6, 5)));
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, untouched (0)));   /* nothing to match */
  t_start (&t);
  other = make_print (&t, 6, 1);   /* enrolled with other settings */
  print = make_print (&t, 6, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  g_assert_false (verify (&t, other, &match, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_assert_false (verify (&t, print, &match, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  t_finish (&t);
}

typedef struct
{
  gboolean suspended, resumed, verified;
  GError  *verify_error;
  gboolean match;
} Async;

static Async async;

static void
resumed (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(GError) error = NULL;

  (void) user_data;
  g_assert_true (fp_device_resume_finish (FP_DEVICE (source), res, &error));
  g_assert_no_error (error);
  async.resumed = TRUE;
}

static void
suspended (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(GError) error = NULL;

  (void) user_data;
  g_assert_true (fp_device_suspend_finish (FP_DEVICE (source), res, &error));
  g_assert_no_error (error);
  async.suspended = TRUE;
  fp_device_resume (FP_DEVICE (source), NULL, resumed, NULL);
}

static void
suspend_hook (gpointer data)
{
  fp_device_suspend (((T *) data)->device, NULL, suspended, NULL);
}

static void
verified (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  fp_device_verify_finish (FP_DEVICE (source), res, &async.match, NULL, &async.verify_error);
  async.verified = TRUE;
}

static void
test_suspend_resume_during_verify (void)
{
  T t;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;

  memset (&async, 0, sizeof async);
  t_init (&t);
  t.seq = script_open (&t.s, t.seq);
  t.seq = script_touch_cancelled (&t.s, t.seq, t.settings, suspend_hook, FALSE);
  t.seq = script_open (&t.s, 1);   /* resume initialises again from sequence 1 */
  t.seq = script_touch (&t.s, t.seq, t.settings, keep_stream (&t, touch (7, 5)));
  t_start (&t);
  print = make_print (&t, 7, 0);
  g_assert_true (fp_device_open_sync (t.device, NULL, &error));
  fp_device_verify (t.device, print, t.cancellable, NULL, NULL, NULL, verified, NULL);
  while (!async.verified)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (async.suspended);
  g_assert_true (async.resumed);
  g_assert_no_error (async.verify_error);
  g_assert_true (async.match);
  t_finish (&t);
}

int
main (int argc, char **argv)
{
  make_background ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sidetouch/device/failed-open-discards", test_failed_open_discards);
  g_test_add_func ("/sidetouch/device/enrol-verify-identify", test_enrol_verify_identify);
  g_test_add_func ("/sidetouch/device/fprintd-enrol-sequence", test_fprintd_enrol_sequence);
  g_test_add_func ("/sidetouch/device/reference-retry-and-failure", test_reference_retry_and_failure);
  g_test_add_func ("/sidetouch/device/reference-status-guard", test_reference_status_guard);
  g_test_add_func ("/sidetouch/device/settings-change-during-enrolment", test_settings_change_during_enrolment);
  g_test_add_func ("/sidetouch/device/cancel-then-reuse", test_cancel_then_reuse);
  g_test_add_func ("/sidetouch/device/unknown-reader-refuses", test_unknown_reader_refuses);
  g_test_add_func ("/sidetouch/device/transport-failure-during-wait", test_transport_failure_during_wait);
  g_test_add_func ("/sidetouch/device/foreign-print-refused", test_foreign_print_refused);
  g_test_add_func ("/sidetouch/device/settings-and-unscorable", test_settings_and_unscorable);
  g_test_add_func ("/sidetouch/device/suspend-resume-during-verify", test_suspend_resume_during_verify);
  return g_test_run ();
}
