/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The BIO-key SideTouch (2963:2001) libfprint device. See sidetouch.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch.h"
#include "sidetouch-acquire.h"
#include "sidetouch-match.h"
#include "sidetouch-print.h"
#include "sidetouch-reference.h"
#include "sidetouch-usb.h"
#include "sidetouch-wipe.h"

#include <math.h>
#include <string.h>

#define TIMEOUT_MS 1000
#define FIRST_SEQUENCE 1
#define REFERENCE_PAUSE_MS 1500
#define REFERENCE_ATTEMPTS 3

typedef struct
{
  uint16_t touches[SIDETOUCH_ENROLL_TOUCHES][SIDETOUCH_PIXELS];
  uint16_t frames[SIDETOUCH_REFERENCE_FRAMES][SIDETOUCH_PIXELS];
  uint16_t frame_status[SIDETOUCH_REFERENCE_FRAMES];   /* each frame's 01 43 word, for G1 */
  uint16_t reference[SIDETOUCH_PIXELS];
  uint8_t  settings[SIDETOUCH_SETTINGS_LENGTH];
  int      touch_count;
  int      frame_count;
  int      attempts;
} Enrolment;

typedef struct
{
  SidetouchTransport           transport;
  const SidetouchInterfaceOps *interface;
  gpointer                     interface_backend;
  guint16                      next_sequence;
  gboolean                     reader_unknown;  /* until the next open */

  /* the current action */
  SidetouchAcquire            *acquire;
  gboolean                     waiting;         /* touch mode, armed, before contact */
  gboolean                     cancelled;
  gboolean                     suspend_pending;
  gboolean                     suspended;
  GCancellable                *pause;
  Enrolment                   *enrol;
  GPtrArray                   *prints;          /* SidetouchPrint *, verify or identify */
  GPtrArray                   *gallery;         /* the matching FpPrint *, borrowed */
} FpiDeviceSidetouchPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (FpiDeviceSidetouch, fpi_device_sidetouch, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x2963, .pid = 0x2001 },
  { .vid = 0, .pid = 0 },
};

#define PRIV(self) ((FpiDeviceSidetouchPrivate *) fpi_device_sidetouch_get_instance_private (self))

static void start_touch (FpiDeviceSidetouch *self);
static void reference_start (FpiDeviceSidetouch *self);

/* ---- helpers ------------------------------------------------------------- */

/* A serialised print: wiped when released. */
typedef struct
{
  guint8 *bytes;
  gsize   size;
} Serialised;

static void
serialised_free (gpointer data)
{
  Serialised *z = data;

  sidetouch_wipe (z->bytes, z->size);
  g_free (z->bytes);
  g_free (z);
}

static void
print_free (gpointer data)
{
  SidetouchPrint *p = data;

  sidetouch_print_clear (p);
  g_free (p);
}

static void
action_cleanup (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  if (priv->enrol)
    {
      sidetouch_wipe (priv->enrol, sizeof *priv->enrol);
      g_clear_pointer (&priv->enrol, g_free);
    }
  g_clear_pointer (&priv->prints, g_ptr_array_unref);
  g_clear_pointer (&priv->gallery, g_ptr_array_unref);
  g_clear_object (&priv->pause);
  priv->acquire = NULL;
  priv->waiting = FALSE;
}

static void
action_error (FpiDeviceSidetouch *self, GError *error)
{
  action_cleanup (self);
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);
  fpi_device_action_error (FP_DEVICE (self), error);
}

static GError *
cancelled_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
}

/* TRUE (and the action ended) if the action was cancelled meanwhile. */
static gboolean
check_cancelled (FpiDeviceSidetouch *self)
{
  if (!PRIV (self)->cancelled)
    return FALSE;
  action_error (self, cancelled_error ());
  return TRUE;
}

/* TRUE if the action may start; otherwise it has been ended. */
static gboolean
action_begin (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->cancelled = FALSE;
  priv->suspend_pending = FALSE;
  priv->suspended = FALSE;
  if (priv->reader_unknown)
    {
      action_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                    "SideTouch reader state unknown; if the next attempt fails, "
                                                    "reconnect the reader"));
      return FALSE;
    }
  return TRUE;
}

static SidetouchPrint *
load_print (FpPrint *print, GError **error)
{
  g_autoptr(GVariant) data = NULL;
  SidetouchPrint *p;
  const guint8 *bytes;
  gsize length = 0;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE_BYTESTRING))
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID, "Not a SideTouch print");
      return NULL;
    }
  bytes = g_variant_get_fixed_array (data, &length, 1);
  p = g_new0 (SidetouchPrint, 1);
  if (sidetouch_print_deserialise (bytes, length, p) != SIDETOUCH_PRINT_OK)
    {
      g_free (p);
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                           "SideTouch print is malformed or of another version");
      return NULL;
    }
  return p;
}

/* The finger a stored print was enrolled for, for the log: a stable name
 * across actions, where the print's index is local to one action. */
static const char *
print_finger (GPtrArray *gallery, guint i)
{
  FpPrint *print = gallery && i < gallery->len ? g_ptr_array_index (gallery, i) : NULL;
  GEnumClass *klass;
  GEnumValue *v;
  const char *name;

  if (!print)
    return "unknown";
  /* a static enum type: its value names outlive the class reference */
  klass = g_type_class_ref (FP_TYPE_FINGER);
  v = g_enum_get_value (klass, (gint) fp_print_get_finger (print));
  name = v ? v->value_nick : "unknown";
  g_type_class_unref (klass);
  return name;
}

/* ---- touch acquisition ----------------------------------------------------- */

static void
touch_armed (FpDevice *device, gpointer user_data)
{
  (void) user_data;
  PRIV (FPI_DEVICE_SIDETOUCH (device))->waiting = TRUE;
  fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_NONE);
}

static void
touch_contact (FpDevice *device, gpointer user_data)
{
  (void) user_data;
  PRIV (FPI_DEVICE_SIDETOUCH (device))->waiting = FALSE;
  fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);
}

static void enrol_touch (FpiDeviceSidetouch *self, const SidetouchAcquisition *result);
static void match_start (FpiDeviceSidetouch *self, const SidetouchAcquisition *result);

static void
touch_done (FpDevice *device, const SidetouchAcquisition *result, GError *error, SidetouchReaderState reader,
            gpointer user_data)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  (void) user_data;
  priv->acquire = NULL;
  priv->waiting = FALSE;
  fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
  if (reader == SIDETOUCH_READER_UNKNOWN)
    priv->reader_unknown = TRUE;

  if (priv->suspend_pending)
    {
      priv->suspend_pending = FALSE;
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && reader == SIDETOUCH_READER_KNOWN &&
          !priv->cancelled)
        {
          /* the wait was stopped cleanly for the suspend; the action stays
           * pending and resume restarts it */
          g_clear_error (&error);
          priv->suspended = TRUE;
          fpi_device_suspend_complete (device, NULL);
          return;
        }
      fpi_device_suspend_complete (device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
    }

  if (error)
    {
      action_error (self, error);
      return;
    }
  if (check_cancelled (self))
    return;

  switch (fpi_device_get_current_action (device))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      enrol_touch (self, result);
      return;

    case FPI_DEVICE_ACTION_VERIFY:
    case FPI_DEVICE_ACTION_IDENTIFY:
      match_start (self, result);
      return;

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_CAPTURE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      action_error (self, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }
}

static const SidetouchAcquireCallbacks touch_callbacks = { touch_armed, touch_contact, touch_done };

static void
start_touch (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->waiting = FALSE;
  priv->acquire = sidetouch_acquire_start (FP_DEVICE (self), &priv->transport, &priv->next_sequence,
                                           SIDETOUCH_MODE_TOUCH, TIMEOUT_MS, &touch_callbacks, NULL);
}

/* ---- enrolment -------------------------------------------------------------- */

static void
enrol_touch (FpiDeviceSidetouch *self, const SidetouchAcquisition *result)
{
  Enrolment *e = PRIV (self)->enrol;

  if (e->touch_count == 0)
    memcpy (e->settings, result->settings, sizeof e->settings);
  else if (!sidetouch_settings_match (e->settings, result->settings))
    {
      action_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                    "SideTouch reader settings changed during enrolment"));
      return;
    }
  memcpy (e->touches[e->touch_count++], result->samples, sizeof e->touches[0]);
  fpi_device_enroll_progress (FP_DEVICE (self), e->touch_count, NULL, NULL);
  if (e->touch_count < SIDETOUCH_ENROLL_TOUCHES)
    start_touch (self);
  else
    reference_start (self);
}

/* The template and the serialised print, off the main loop. */
static void
enrol_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  Enrolment *e = task_data;
  const uint16_t *touches[SIDETOUCH_ENROLL_TOUCHES];
  SidetouchPrint *print = g_new0 (SidetouchPrint, 1);
  SidetouchMatchResult r;
  gsize size;
  guint8 *bytes;
  int dropped = 0;

  (void) source;
  (void) cancellable;
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    touches[i] = e->touches[i];
  memcpy (print->settings, e->settings, sizeof print->settings);
  memcpy (print->reference, e->reference, sizeof print->reference);
  r = sidetouch_match_enrol (touches, print->reference, &print->template, &dropped);
  if (r != SIDETOUCH_MATCH_OK)
    {
      print_free (print);
      g_task_return_error (task, fpi_device_error_new_msg (
                             r == SIDETOUCH_MATCH_TOO_FEW ? FP_DEVICE_ERROR_DATA_INVALID : FP_DEVICE_ERROR_GENERAL,
                             r == SIDETOUCH_MATCH_TOO_FEW ? "Fewer than three usable touches; enrol again"
                                                          : "SideTouch could not build the template"));
      return;
    }
  if (dropped)
    fp_info ("SideTouch enrolment: %d of %d touches unusable, dropped", dropped, SIDETOUCH_ENROLL_TOUCHES);
  size = sidetouch_print_serialised_size (print);
  bytes = size ? g_malloc (size) : NULL;
  if (!bytes || sidetouch_print_serialise (print, bytes, size) != size)
    {
      if (bytes)
        sidetouch_wipe (bytes, size);
      g_free (bytes);
      print_free (print);
      g_task_return_error (task, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "SideTouch could not store the print"));
      return;
    }
  print_free (print);
  {
    Serialised *z = g_new (Serialised, 1);

    z->bytes = bytes;
    z->size = size;
    g_task_return_pointer (task, z, serialised_free);
  }
}

static void
enrol_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (source);
  GError *error = NULL;
  Serialised *z = g_task_propagate_pointer (G_TASK (res), &error);
  FpPrint *print = NULL;

  (void) user_data;
  if (check_cancelled (self))
    {
      g_clear_error (&error);
      if (z)
        serialised_free (z);
      return;
    }
  if (error)
    {
      action_error (self, error);
      return;
    }
  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, z->bytes, z->size, 1), NULL);
  serialised_free (z);
  action_cleanup (self);
  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
}

/* A rejected attempt, by a guard or because the reader refused a frame:
 * retry after the pause, or fail the enrolment after the last attempt. */
static void
reference_rejected (FpiDeviceSidetouch *self)
{
  Enrolment *e = PRIV (self)->enrol;

  sidetouch_wipe (e->frames, sizeof e->frames);
  sidetouch_wipe (e->reference, sizeof e->reference);
  if (e->attempts < REFERENCE_ATTEMPTS)
    {
      fpi_device_enroll_progress (FP_DEVICE (self), e->touch_count, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      reference_start (self);
      return;
    }
  fp_info ("SideTouch could not build its background reference in %d attempts; "
           "keep the finger off the sensor after the last touch", REFERENCE_ATTEMPTS);
  action_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                "The reader could not be calibrated: keep the finger off the "
                                                "sensor after the last touch, then enrol again"));
}

/* All three guards are evaluated and logged on every complete attempt, so
 * that each one's behaviour is visible; any failure rejects the attempt. */
static void
reference_check (FpiDeviceSidetouch *self)
{
  Enrolment *e = PRIV (self)->enrol;
  const uint16_t *frames[SIDETOUCH_REFERENCE_FRAMES] = { e->frames[0], e->frames[1], e->frames[2] };
  const uint16_t *touches[SIDETOUCH_ENROLL_TOUCHES];
  SidetouchReferenceStats stats = { 0 };
  SidetouchReferenceResult g2, g3;
  gboolean g1 = TRUE;
  GTask *task;

  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    touches[i] = e->touches[i];
  for (int f = 0; f < SIDETOUCH_REFERENCE_FRAMES; f++)
    g1 = g1 && sidetouch_reference_status_ok (e->frame_status[f]);
  g2 = sidetouch_reference_build (frames, e->reference, &stats);
  g3 = g2 == SIDETOUCH_REFERENCE_INVALID ? g2 : sidetouch_reference_check_level (e->reference, touches, &stats);
  fp_info ("SideTouch reference attempt %d: status 0x%04x 0x%04x 0x%04x (G1 %s); spread %d, mean range %.2f "
           "(G2 %s); level %.2f (G3 %s): %s",
           e->attempts, e->frame_status[0], e->frame_status[1], e->frame_status[2], g1 ? "pass" : "fail",
           stats.max_spread, stats.mean_range,
           g2 == SIDETOUCH_REFERENCE_OK ? "pass"
           : g2 == SIDETOUCH_REFERENCE_SPREAD ? "fail, spread"
           : g2 == SIDETOUCH_REFERENCE_MEAN_RANGE ? "fail, means" : "invalid",
           stats.level, g3 == SIDETOUCH_REFERENCE_OK ? "pass" : g3 == SIDETOUCH_REFERENCE_LEVEL ? "fail" : "invalid",
           g1 && g2 == SIDETOUCH_REFERENCE_OK && g3 == SIDETOUCH_REFERENCE_OK ? "accepted" : "rejected");
  sidetouch_wipe (e->frames, sizeof e->frames);

  if (!g1 || g2 != SIDETOUCH_REFERENCE_OK || g3 != SIDETOUCH_REFERENCE_OK)
    {
      reference_rejected (self);
      return;
    }

  task = g_task_new (self, NULL, enrol_finished, NULL);
  g_task_set_task_data (task, e, NULL);   /* e lives until the action ends */
  g_task_run_in_thread (task, enrol_thread);
  g_object_unref (task);
}

static void frame_start (FpiDeviceSidetouch *self);

static void
frame_done (FpDevice *device, const SidetouchAcquisition *result, GError *error, SidetouchReaderState reader,
            gpointer user_data)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);
  FpiDeviceSidetouchPrivate *priv = PRIV (self);
  Enrolment *e = priv->enrol;

  (void) user_data;
  priv->acquire = NULL;
  if (reader == SIDETOUCH_READER_UNKNOWN)
    priv->reader_unknown = TRUE;
  if (g_error_matches (error, SIDETOUCH_ACQUIRE_ERROR, SIDETOUCH_ACQUIRE_ERROR_REFUSED))
    {
      fp_info ("SideTouch reference attempt %d, frame %d: %s; attempt rejected", e->attempts, e->frame_count + 1,
               error->message);
      g_clear_error (&error);
      if (check_cancelled (self))
        return;
      reference_rejected (self);
      return;
    }
  if (error)
    {
      action_error (self, error);
      return;
    }
  if (check_cancelled (self))
    return;
  e->frame_status[e->frame_count] = result->status;
  memcpy (e->frames[e->frame_count++], result->samples, sizeof e->frames[0]);
  if (e->frame_count < SIDETOUCH_REFERENCE_FRAMES)
    frame_start (self);
  else
    reference_check (self);
}

static const SidetouchAcquireCallbacks frame_callbacks = { NULL, NULL, frame_done };

static void
frame_start (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->acquire = sidetouch_acquire_start (FP_DEVICE (self), &priv->transport, &priv->next_sequence,
                                           SIDETOUCH_MODE_UNTOUCHED, TIMEOUT_MS, &frame_callbacks, NULL);
}

static void
pause_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiDeviceSidetouch *self = user_data;

  (void) data;
  (void) length;
  g_clear_object (&PRIV (self)->pause);
  if (error && !PRIV (self)->cancelled)
    {
      action_error (self, error);
      return;
    }
  g_clear_error (&error);
  if (check_cancelled (self))
    return;
  PRIV (self)->enrol->frame_count = 0;
  frame_start (self);
}

/* After the last touch, or after a rejected attempt: pause, then three
 * untouched acquisitions. */
static void
reference_start (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->enrol->attempts++;
  fp_info ("SideTouch: building the background reference (attempt %d of %d); keep the finger off the sensor",
           priv->enrol->attempts, REFERENCE_ATTEMPTS);
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);
  priv->pause = g_cancellable_new ();
  priv->transport.ops->wait (priv->transport.backend, REFERENCE_PAUSE_MS, priv->pause, pause_done, self);
}

static void
sidetouch_enroll (FpDevice *device)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);

  if (!action_begin (self))
    return;
  PRIV (self)->enrol = g_new0 (Enrolment, 1);
  start_touch (self);
}

/* ---- verification and identification ------------------------------------------ */

typedef struct
{
  uint16_t   probe[SIDETOUCH_PIXELS];
  GPtrArray *prints;     /* SidetouchPrint *, borrowed */
  gboolean  *usable;     /* per print: settings match */
  int       *scores;     /* per print; -1 unscorable or unusable */
} Matching;

static void
matching_free (gpointer data)
{
  Matching *m = data;

  sidetouch_wipe (m->probe, sizeof m->probe);
  g_free (m->usable);
  g_free (m->scores);
  g_free (m);
}

/* The probe's features are extracted once per distinct reference and
 * compared with every template that shares it. */
static void
match_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  Matching *m = task_data;
  guint n = m->prints->len;
  KpSet *features = g_new0 (KpSet, n);
  gboolean *done = g_new0 (gboolean, n);

  (void) source;
  (void) cancellable;
  for (guint i = 0; i < n; i++)
    m->scores[i] = -1;
  for (guint i = 0; i < n; i++)
    {
      SidetouchPrint *p = g_ptr_array_index (m->prints, i);
      gboolean scorable;

      if (!m->usable[i] || done[i])
        continue;
      scorable = sidetouch_match_features (m->probe, p->reference, &features[i]) == SIDETOUCH_MATCH_OK;
      for (guint j = i; j < n; j++)
        {
          SidetouchPrint *q = g_ptr_array_index (m->prints, j);

          if (!m->usable[j] || done[j] || memcmp (q->reference, p->reference, sizeof p->reference) != 0)
            continue;
          done[j] = TRUE;
          if (scorable)
            m->scores[j] = kpt_verify (&q->template, &features[i]).score;
        }
      sidetouch_wipe_set (&features[i]);
    }
  g_free (features);
  g_free (done);
  g_task_return_boolean (task, TRUE);
}

static void
match_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (source);
  FpDevice *device = FP_DEVICE (self);
  Matching *m = g_task_get_task_data (G_TASK (res));
  int best = -1, best_score = -1;
  gboolean any = FALSE;

  (void) user_data;
  if (check_cancelled (self))
    return;
  for (guint i = 0; i < m->prints->len; i++)
    {
      fp_dbg ("SideTouch print %u (%s): score %d", i, print_finger (PRIV (self)->gallery, i), m->scores[i]);
      if (m->scores[i] >= 0)
        any = TRUE;
      if (m->scores[i] > KPT_THRESHOLD && m->scores[i] > best_score)
        best = (int) i, best_score = m->scores[i];
    }

  if (fpi_device_get_current_action (device) == FPI_DEVICE_ACTION_VERIFY)
    {
      if (!any)
        fpi_device_verify_report (device, FPI_MATCH_ERROR, NULL, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      else
        fpi_device_verify_report (device, best >= 0 ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL, NULL, NULL);
      action_cleanup (self);
      fpi_device_verify_complete (device, NULL);
      return;
    }
  if (!any)
    fpi_device_identify_report (device, NULL, NULL, fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
  else
    fpi_device_identify_report (device, best >= 0 ? g_ptr_array_index (PRIV (self)->gallery, best) : NULL, NULL,
                                NULL);
  action_cleanup (self);
  fpi_device_identify_complete (device, NULL);
}

static void
match_start (FpiDeviceSidetouch *self, const SidetouchAcquisition *result)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);
  Matching *m = g_new0 (Matching, 1);
  guint usable = 0;
  GTask *task;

  memcpy (m->probe, result->samples, sizeof m->probe);
  m->prints = priv->prints;
  m->usable = g_new0 (gboolean, priv->prints->len);
  m->scores = g_new0 (int, priv->prints->len);
  for (guint i = 0; i < priv->prints->len; i++)
    {
      SidetouchPrint *p = g_ptr_array_index (priv->prints, i);

      m->usable[i] = sidetouch_settings_match (p->settings, result->settings);
      if (m->usable[i])
        usable++;
      if (!m->usable[i])
        fp_info ("SideTouch print %u was enrolled with other reader settings; not compared", i);
    }
  if (!usable)
    {
      matching_free (m);
      action_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                    "The print was enrolled with other reader settings"));
      return;
    }
  task = g_task_new (self, NULL, match_finished, NULL);
  g_task_set_task_data (task, m, matching_free);
  g_task_run_in_thread (task, match_thread);
  g_object_unref (task);
}

/* Diagnostic only: drift between the references of the prints offered for
 * identification. It never affects a decision. */
static void
log_reference_drift (GPtrArray *prints, GPtrArray *gallery)
{
  for (guint i = 0; i < prints->len; i++)
    for (guint j = i + 1; j < prints->len; j++)
      {
        const SidetouchPrint *a = g_ptr_array_index (prints, i), *b = g_ptr_array_index (prints, j);
        double sum = 0, squares = 0, mean;

        if (memcmp (a->reference, b->reference, sizeof a->reference) == 0)
          continue;
        for (int k = 0; k < SIDETOUCH_PIXELS; k++)
          sum += (double) a->reference[k] - (double) b->reference[k];
        mean = sum / SIDETOUCH_PIXELS;
        for (int k = 0; k < SIDETOUCH_PIXELS; k++)
          {
            double d = (double) a->reference[k] - (double) b->reference[k] - mean;

            squares += d * d;
          }
        fp_info ("SideTouch reference drift, prints %u (%s) and %u (%s): mean offset %.2f, SD %.2f", i,
                 print_finger (gallery, i), j, print_finger (gallery, j), mean, sqrt (squares / SIDETOUCH_PIXELS));
      }
}

static void
sidetouch_verify (FpDevice *device)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);
  FpiDeviceSidetouchPrivate *priv = PRIV (self);
  GError *error = NULL;
  FpPrint *print = NULL;
  SidetouchPrint *p;

  if (!action_begin (self))
    return;
  fpi_device_get_verify_data (device, &print);
  p = load_print (print, &error);
  if (!p)
    {
      action_error (self, error);
      return;
    }
  priv->prints = g_ptr_array_new_with_free_func (print_free);
  g_ptr_array_add (priv->prints, p);
  priv->gallery = g_ptr_array_new ();
  g_ptr_array_add (priv->gallery, print);
  start_touch (self);
}

static void
sidetouch_identify (FpDevice *device)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);
  FpiDeviceSidetouchPrivate *priv = PRIV (self);
  GPtrArray *gallery = NULL;

  if (!action_begin (self))
    return;
  fpi_device_get_identify_data (device, &gallery);
  if (!gallery || !gallery->len)
    {
      /* Nothing to compare with, so nothing can match and no touch is needed.
       * fprintd sends an empty gallery as its duplicate check before the
       * first enrolment, and fails the enrolment on an error there. */
      fp_info ("SideTouch identify: empty gallery, no match");
      fpi_device_identify_report (device, NULL, NULL, NULL);
      fpi_device_identify_complete (device, NULL);
      return;
    }
  priv->prints = g_ptr_array_new_with_free_func (print_free);
  priv->gallery = g_ptr_array_new ();
  for (guint i = 0; gallery && i < gallery->len; i++)
    {
      g_autoptr(GError) error = NULL;
      SidetouchPrint *p = load_print (g_ptr_array_index (gallery, i), &error);

      if (!p)
        {
          fp_info ("SideTouch identify: print %u skipped: %s", i, error->message);
          continue;
        }
      g_ptr_array_add (priv->prints, p);
      g_ptr_array_add (priv->gallery, g_ptr_array_index (gallery, i));
    }
  if (!priv->prints->len)
    {
      action_error (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "No usable SideTouch print"));
      return;
    }
  log_reference_drift (priv->prints, priv->gallery);
  start_touch (self);
}

/* ---- lifecycle ------------------------------------------------------------------ */

/* libfprint opens the USB handle before the open vfunc and closes it only at
 * a completed close, which a failed open never reaches: left open, the handle
 * makes every later open fail ("already open") until the process exits. */
static void
open_done (FpDevice *device, GError *error, gpointer user_data)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));

  (void) user_data;
  if (error && priv->interface->discard)
    priv->interface->discard (priv->interface_backend);
  fpi_device_open_complete (device, error);
}

static void
sidetouch_open (FpDevice *device)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));

  priv->reader_unknown = FALSE;
  priv->next_sequence = FIRST_SEQUENCE;
  sidetouch_open_async (device, &priv->transport, priv->interface, priv->interface_backend, &priv->next_sequence,
                        TIMEOUT_MS, fpi_device_get_cancellable (device), open_done, NULL);
}

static void
sidetouch_close (FpDevice *device)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));

  priv->interface->release (priv->interface_backend);
  fpi_device_close_complete (device, NULL);
}

static void
sidetouch_cancel (FpDevice *device)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));

  priv->cancelled = TRUE;
  if (priv->acquire)
    sidetouch_acquire_cancel (priv->acquire);
  if (priv->pause)
    g_cancellable_cancel (priv->pause);
  if (priv->suspended)
    {
      /* nothing is in flight while suspended */
      priv->suspended = FALSE;
      action_error (FPI_DEVICE_SIDETOUCH (device), cancelled_error ());
    }
}

/* Only a verification or identification waiting for a finger can be
 * suspended: its wait is stopped through the reader's own cancellation and
 * the action stays pending. Anything else reports unsupported, and the core
 * cancels it. */
static void
sidetouch_suspend (FpDevice *device)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));
  FpiDeviceAction action = fpi_device_get_current_action (device);

  if ((action == FPI_DEVICE_ACTION_VERIFY || action == FPI_DEVICE_ACTION_IDENTIFY) && priv->acquire &&
      priv->waiting)
    {
      priv->suspend_pending = TRUE;
      sidetouch_acquire_cancel (priv->acquire);
      return;
    }
  fpi_device_suspend_complete (device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
}

static gboolean
claimed_already (gpointer backend, GError **error)
{
  (void) backend;
  (void) error;
  return TRUE;
}

static void
release_nothing (gpointer backend)
{
  (void) backend;
}

/* On resume the interface is still claimed; initialisation runs again. */
static const SidetouchInterfaceOps resume_interface = { claimed_already, release_nothing, NULL };

static void
resume_done (FpDevice *device, GError *error, gpointer user_data)
{
  FpiDeviceSidetouch *self = FPI_DEVICE_SIDETOUCH (device);

  (void) user_data;
  fpi_device_resume_complete (device, error ? g_error_copy (error) : NULL);
  if (error)
    {
      action_error (self, error);
      return;
    }
  if (check_cancelled (self))
    return;
  start_touch (self);
}

static void
sidetouch_resume (FpDevice *device)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (FPI_DEVICE_SIDETOUCH (device));

  if (!priv->suspended)
    {
      fpi_device_resume_complete (device, NULL);
      return;
    }
  priv->suspended = FALSE;
  priv->next_sequence = FIRST_SEQUENCE;
  sidetouch_open_async (device, &priv->transport, &resume_interface, NULL, &priv->next_sequence, TIMEOUT_MS, NULL,
                        resume_done, NULL);
}

/* ---- USB interface ------------------------------------------------------------- */

static gboolean
usb_claim (gpointer backend, GError **error)
{
  return g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (backend)), 0, 0, error);
}

static void
usb_release (gpointer backend)
{
  g_autoptr(GError) error = NULL;

  if (!g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (backend)), 0, 0, &error))
    fp_dbg ("SideTouch interface release failed: %s", error->message);
}

static void
usb_discard (gpointer backend)
{
  g_autoptr(GError) error = NULL;

  if (!g_usb_device_close (fpi_device_get_usb_device (FP_DEVICE (backend)), &error))
    fp_dbg ("SideTouch USB close after a failed open failed: %s", error->message);
}

static const SidetouchInterfaceOps usb_interface = { usb_claim, usb_release, usb_discard };

void
fpi_device_sidetouch_set_transport (FpiDeviceSidetouch *self, const SidetouchTransport *transport,
                                    const SidetouchInterfaceOps *interface, gpointer interface_backend)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->transport = *transport;
  priv->interface = interface;
  priv->interface_backend = interface_backend;
}

static void
fpi_device_sidetouch_finalize (GObject *object)
{
  action_cleanup (FPI_DEVICE_SIDETOUCH (object));
  G_OBJECT_CLASS (fpi_device_sidetouch_parent_class)->finalize (object);
}

static void
fpi_device_sidetouch_init (FpiDeviceSidetouch *self)
{
  FpiDeviceSidetouchPrivate *priv = PRIV (self);

  priv->transport.ops = &sidetouch_usb_ops;
  priv->transport.backend = self;
  priv->interface = &usb_interface;
  priv->interface_backend = self;
}

static void
fpi_device_sidetouch_class_init (FpiDeviceSidetouchClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_sidetouch_finalize;
  dev_class->id = "sidetouch";
  dev_class->full_name = "BIO-key SideTouch";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = SIDETOUCH_ENROLL_TOUCHES;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->open = sidetouch_open;
  dev_class->close = sidetouch_close;
  dev_class->enroll = sidetouch_enroll;
  dev_class->verify = sidetouch_verify;
  dev_class->identify = sidetouch_identify;
  dev_class->cancel = sidetouch_cancel;
  dev_class->suspend = sidetouch_suspend;
  dev_class->resume = sidetouch_resume;
  fpi_device_class_auto_initialize_features (dev_class);
}
