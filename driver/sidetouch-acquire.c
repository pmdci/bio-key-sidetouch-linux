/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * One SideTouch acquisition. See sidetouch-acquire.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch-acquire.h"
#include "sidetouch-exchange.h"
#include "sidetouch-wipe.h"

#include <string.h>

#define WAIT_DELAY_MS 50
#define CANCEL_DELAY_MS 60

typedef enum {
  ACQ_RUNTIME,
  ACQ_SETUP,
  ACQ_ARMED,
  ACQ_WAIT_POLL_OUT,
  ACQ_WAIT_POLL_IN,
  ACQ_WAIT_DELAY,
  ACQ_DRAIN,
  ACQ_RESULT,
  ACQ_CLEAR,
  ACQ_DATA_READY,
  ACQ_CHUNK,
  ACQ_DECODE,
  ACQ_CANCEL_SEND,
  ACQ_CANCEL_ACK,
  ACQ_CANCEL_DELAY,
  ACQ_CANCEL_POLL_OUT,
  ACQ_CANCEL_POLL_IN,
  ACQ_CANCEL_DRAIN,
  ACQ_CANCEL_STATUS,
  ACQ_CANCEL_CLEAR,
  ACQ_NUM_STATES,
} AcqState;

struct _SidetouchAcquire
{
  SidetouchTransport        transport;
  guint16                  *next_sequence;
  SidetouchMode             mode;
  guint                     timeout_ms;
  GCancellable             *work;          /* cancelled by sidetouch_acquire_cancel */
  gboolean                  cancel_requested;
  gboolean                  past_boundary;
  gboolean                  in_cancel_sequence;
  GError                   *cancel_error;  /* reported once the cancel sequence completes */
  gint64                    deadline;      /* untouched mode only */
  GCancellable             *expiry;        /* untouched mode: cancelled at the deadline */
  GSource                  *expiry_timer;
  gboolean                  expired;
  guint                     setup_index;
  unsigned                  attempts;
  uint16_t                  status;
  uint8_t                   error_byte;    /* untouched mode: from the 01 43 reply */
  gboolean                  refused;       /* untouched mode: the reader refused the frame */
  gboolean                  refuse_after_cancel; /* untouched mode: a wait stopped at the deadline */
  uint8_t                   settings[SIDETOUCH_SETTINGS_BYTES];
  SidetouchChunks           chunks;
  uint8_t                   raw[SIDETOUCH_ACQUISITION_LENGTH];
  uint16_t                  samples[SIDETOUCH_PIXELS];
  SidetouchAcquireCallbacks callbacks;
  gpointer                  user_data;
};

static void
acquire_free (SidetouchAcquire *self)
{
  g_clear_object (&self->work);
  if (self->expiry_timer)
    g_source_destroy (self->expiry_timer);
  g_clear_object (&self->expiry);
  g_clear_error (&self->cancel_error);
  /* the stream and samples are sensor data */
  sidetouch_wipe (self->raw, sizeof self->raw);
  sidetouch_wipe (self->samples, sizeof self->samples);
  g_free (self);
}

static GError *
proto_error (FpiSsm *ssm, const char *what)
{
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "SideTouch acquisition: %s (state %d)", what,
                                   fpi_ssm_get_cur_state (ssm));
}

G_DEFINE_QUARK (sidetouch-acquire-error-quark, sidetouch_acquire_error)

/* Untouched mode: the reader refused the frame. Its flag register has been
 * cleared, so the reader state stays known. */
static void
refuse (FpiSsm *ssm, SidetouchAcquire *self, const char *word_name, uint16_t word)
{
  self->refused = TRUE;
  fpi_ssm_mark_failed (ssm, g_error_new (SIDETOUCH_ACQUIRE_ERROR, SIDETOUCH_ACQUIRE_ERROR_REFUSED,
                                         "SideTouch untouched frame refused: %s 0x%04x, error byte 0x%02x",
                                         word_name, word, self->error_byte));
}

/* Untouched mode: a wait poll and its status reply, which always complete
 * (bounded by the transfer timeout) so that a wait still pending at the
 * deadline stops at a clean point, between polls. */
static gboolean
untouched_wait_poll (FpiSsm *ssm, SidetouchAcquire *self)
{
  int state = fpi_ssm_get_cur_state (ssm);

  return self->mode == SIDETOUCH_MODE_UNTOUCHED && !self->in_cancel_sequence &&
         (state == ACQ_WAIT_POLL_OUT || state == ACQ_WAIT_POLL_IN);
}

/* The work cancellable while the user may still cancel; none afterwards, so
 * that a deferred cancellation and the cancel sequence itself run to the end.
 * Untouched mode uses its deadline's cancellable, so a transfer in flight at
 * the deadline is stopped, except for the wait polls and the cancel sequence. */
static GCancellable *
cancellable (FpiSsm *ssm, SidetouchAcquire *self)
{
  if (self->in_cancel_sequence)
    return NULL;
  if (self->mode == SIDETOUCH_MODE_UNTOUCHED)
    return untouched_wait_poll (ssm, self) ? NULL : self->expiry;
  return self->past_boundary ? NULL : self->work;
}

/* The monotonic deadline decides: a device timeout takes its start from the
 * main context's cached time, which can lag after a long dispatch, so an
 * early firing re-arms for the time actually left. */
static void
expire (FpDevice *device, gpointer user_data)
{
  SidetouchAcquire *self = user_data;
  gint64 left = self->deadline - g_get_monotonic_time ();

  self->expiry_timer = NULL;
  if (left > 0)
    {
      self->expiry_timer = fpi_device_add_timeout (device, (gint) ((left + 999) / 1000), expire, self, NULL);
      return;
    }
  self->expired = TRUE;
  g_cancellable_cancel (self->expiry);
}

static GError *
deadline_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "SideTouch untouched acquisition deadline passed");
}

/* The cancellation sequence that stops a wait at the deadline runs on its
 * own transfer timeouts. */
static gboolean
deadline_passed (SidetouchAcquire *self)
{
  return self->mode == SIDETOUCH_MODE_UNTOUCHED && !self->in_cancel_sequence &&
         (self->expired || g_get_monotonic_time () >= self->deadline);
}

/* Transfer timeout; in untouched mode never beyond the whole-acquisition
 * deadline, except for a wait poll and the cancel sequence. 0 means the
 * deadline has passed. */
static guint
transfer_timeout (FpiSsm *ssm, SidetouchAcquire *self)
{
  gint64 left;

  if (self->mode == SIDETOUCH_MODE_TOUCH || self->in_cancel_sequence || untouched_wait_poll (ssm, self))
    return self->timeout_ms;
  left = (self->deadline - g_get_monotonic_time () + 999) / 1000;
  if (left <= 0)
    return 0;
  return (guint) MIN ((gint64) self->timeout_ms, left);
}

/* Untouched mode: the reader still reports the wait pending at the deadline.
 * It holds mode 3 while a finger rests on the sensor (a live capture: every
 * poll pending for the whole deadline, with the finger resting), so the wait
 * is stopped through the reader's cancellation sequence and the frame counts
 * as refused. A failed sequence leaves the reader state unknown. */
static void
stop_pending_wait (FpiSsm *ssm, SidetouchAcquire *self)
{
  self->in_cancel_sequence = TRUE;
  self->refuse_after_cancel = TRUE;
  self->cancel_error = g_error_new (SIDETOUCH_ACQUIRE_ERROR, SIDETOUCH_ACQUIRE_ERROR_REFUSED,
                                    "SideTouch untouched wait still pending after %d ms (a finger on the sensor)",
                                    SIDETOUCH_UNTOUCHED_DEADLINE_MS);
  fpi_ssm_jump_to_state (ssm, ACQ_CANCEL_SEND);
}

/* The deadline as found at a state's start: in the wait, before a poll or a
 * delay, it stops the wait; anywhere else it fails the acquisition. */
static void
deadline_reached (FpiSsm *ssm, SidetouchAcquire *self)
{
  int state = fpi_ssm_get_cur_state (ssm);

  if (state == ACQ_WAIT_POLL_OUT || state == ACQ_WAIT_DELAY)
    stop_pending_wait (ssm, self);
  else
    fpi_ssm_mark_failed (ssm, deadline_error ());
}

static void
fail (FpiSsm *ssm, GError *error)
{
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);

  if (self->expired && !self->in_cancel_sequence && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      if (fpi_ssm_get_cur_state (ssm) == ACQ_WAIT_DELAY)
        {
          /* the delay between two pending polls, stopped by the deadline */
          stop_pending_wait (ssm, self);
          return;
        }
      error = deadline_error ();
    }
  if (self->cancel_requested && !self->past_boundary && !self->in_cancel_sequence &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self->setup_index > 0)
    {
      /* a touch wait interrupted before contact: run the reader's own
       * cancellation sequence, then report the cancellation */
      self->cancel_error = error;
      self->in_cancel_sequence = TRUE;
      fpi_ssm_jump_to_state (ssm, ACQ_CANCEL_SEND);
      return;
    }
  fpi_ssm_mark_failed (ssm, error);
}

static gboolean
check_deadline (FpiSsm *ssm, SidetouchAcquire *self)
{
  if (transfer_timeout (ssm, self) && !deadline_passed (self))
    return TRUE;
  deadline_reached (ssm, self);
  return FALSE;
}

static void
exchange_done (FpDevice *device, const guint8 *payload, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);
  uint16_t word = 0;
  int state = fpi_ssm_get_cur_state (ssm);

  (void) device;
  if (error)
    {
      if (g_error_matches (error, SIDETOUCH_EXCHANGE_ERROR, SIDETOUCH_EXCHANGE_ERROR_EMPTY_RESPONSE))
        {
          g_clear_error (&error);
          error = proto_error (ssm, "empty response");
        }
      fail (ssm, error);
      return;
    }

  switch ((AcqState) state)
    {
    case ACQ_RUNTIME:
      if (!sidetouch_runtime_info_ok (payload, length))
        break;
      memcpy (self->settings, payload, sizeof self->settings);
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_SETUP:
      if (!sidetouch_zero_status (payload, length))
        break;
      if (self->setup_index < 3)
        fpi_ssm_jump_to_state (ssm, ACQ_SETUP);
      else
        fpi_ssm_next_state (ssm);
      return;

    case ACQ_RESULT:
      if (self->mode == SIDETOUCH_MODE_UNTOUCHED)
        {
          /* the clear follows every well-formed reply; the word is judged after it */
          if (sidetouch_capture_reply (payload, length, &word, &self->error_byte) != SIDETOUCH_PROTO_OK)
            break;
          self->status = word;
          fpi_ssm_next_state (ssm);
          return;
        }
      if (sidetouch_capture_status (payload, length, &word) != SIDETOUCH_PROTO_OK || !sidetouch_touch_status_ok (word))
        break;
      self->status = word;
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_CLEAR:
      if (!sidetouch_zero_status (payload, length))
        break;
      if (self->mode == SIDETOUCH_MODE_UNTOUCHED && !sidetouch_untouched_status_accepted (self->status, self->error_byte))
        {
          refuse (ssm, self, "status", self->status);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_DATA_READY:
      if (sidetouch_data_ready_status (payload, length, &word) != SIDETOUCH_PROTO_OK)
        break;
      if (self->mode == SIDETOUCH_MODE_UNTOUCHED && !sidetouch_untouched_ready_ok (word))
        {
          refuse (ssm, self, "data-ready", word);
          return;
        }
      if (self->mode == SIDETOUCH_MODE_TOUCH && !sidetouch_touch_status_ok (word))
        break;
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_CHUNK:
      if (sidetouch_chunks_append (&self->chunks, payload, length) != SIDETOUCH_PROTO_OK)
        break;
      if (sidetouch_chunks_complete (&self->chunks))
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_jump_to_state (ssm, ACQ_CHUNK);
      return;

    case ACQ_CANCEL_STATUS:
      if (!sidetouch_cancelled_status_ok (payload, length))
        break;
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_CANCEL_CLEAR:
      if (!sidetouch_zero_status (payload, length))
        break;
      /* a wait stopped at the deadline: the reader confirmed the stop, so its
       * state is known and the frame counts as refused */
      if (self->refuse_after_cancel)
        self->refused = TRUE;
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&self->cancel_error));
      return;

    case ACQ_ARMED:
    case ACQ_WAIT_POLL_OUT:
    case ACQ_WAIT_POLL_IN:
    case ACQ_WAIT_DELAY:
    case ACQ_DRAIN:
    case ACQ_DECODE:
    case ACQ_CANCEL_SEND:
    case ACQ_CANCEL_ACK:
    case ACQ_CANCEL_DELAY:
    case ACQ_CANCEL_POLL_OUT:
    case ACQ_CANCEL_POLL_IN:
    case ACQ_CANCEL_DRAIN:
    case ACQ_NUM_STATES:
    default:
      fpi_ssm_mark_failed (ssm, proto_error (ssm, "unexpected exchange completion"));
      return;
    }
  fpi_ssm_mark_failed (ssm, proto_error (ssm, "response failed validation"));
}

static void
exchange (FpiSsm *ssm, SidetouchAcquire *self, const guint8 *payload, gsize length)
{
  guint timeout = transfer_timeout (ssm, self);

  if (!timeout)
    {
      check_deadline (ssm, self);
      return;
    }
  sidetouch_exchange_async (fpi_ssm_get_device (ssm), &self->transport, (*self->next_sequence)++, payload,
                            length, timeout, cancellable (ssm, self), exchange_done, ssm);
}

static void
transfer_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);
  uint8_t state = 0;

  if (error)
    {
      fail (ssm, error);
      return;
    }

  switch ((AcqState) fpi_ssm_get_cur_state (ssm))
    {
    case ACQ_WAIT_POLL_IN:
    case ACQ_CANCEL_ACK:
    case ACQ_CANCEL_POLL_IN:
      switch (sidetouch_poll_progress (data, length, &self->attempts, &state))
        {
        case SIDETOUCH_POLL_READY:
          break;

        case SIDETOUCH_POLL_RETRY:
          fpi_ssm_jump_to_state (ssm, fpi_ssm_get_cur_state (ssm));
          return;

        case SIDETOUCH_POLL_EXHAUSTED:
        case SIDETOUCH_POLL_INVALID:
        default:
          fpi_ssm_mark_failed (ssm, proto_error (ssm, "invalid or missing serial status"));
          return;
        }
      if (fpi_ssm_get_cur_state (ssm) == ACQ_CANCEL_ACK)
        fpi_ssm_next_state (ssm);
      else if (fpi_ssm_get_cur_state (ssm) == ACQ_CANCEL_POLL_IN)
        {
          if (sidetouch_cancel_state_ok (state))
            fpi_ssm_next_state (ssm);
          else
            fpi_ssm_mark_failed (ssm, proto_error (ssm, "unexpected cancellation state"));
        }
      else
        switch (sidetouch_wait_state (state))
          {
          case SIDETOUCH_WAIT_PENDING:
            fpi_ssm_jump_to_state (ssm, ACQ_WAIT_DELAY);
            return;

          case SIDETOUCH_WAIT_READY:
            /* the contact boundary (touch mode); untouched mode is past it already */
            self->past_boundary = TRUE;
            fpi_ssm_jump_to_state (ssm, ACQ_DRAIN);
            return;
          }
      return;

    case ACQ_WAIT_DELAY:
      fpi_ssm_jump_to_state (ssm, ACQ_WAIT_POLL_OUT);
      return;

    case ACQ_RUNTIME:
    case ACQ_SETUP:
    case ACQ_ARMED:
    case ACQ_WAIT_POLL_OUT:
    case ACQ_DRAIN:
    case ACQ_RESULT:
    case ACQ_CLEAR:
    case ACQ_DATA_READY:
    case ACQ_CHUNK:
    case ACQ_DECODE:
    case ACQ_CANCEL_SEND:
    case ACQ_CANCEL_DELAY:
    case ACQ_CANCEL_POLL_OUT:
    case ACQ_CANCEL_DRAIN:
    case ACQ_CANCEL_STATUS:
    case ACQ_CANCEL_CLEAR:
    case ACQ_NUM_STATES:
    default:
      fpi_ssm_next_state (ssm);
      return;
    }
}

static void
drain_done (FpDevice *device, const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);

  if (error)
    {
      fail (ssm, error);
      return;
    }
  if (!sidetouch_ready_drain_ok (data, length))
    {
      fpi_ssm_mark_failed (ssm, proto_error (ssm, "unexpected ready drain"));
      return;
    }
  if (fpi_ssm_get_cur_state (ssm) == ACQ_DRAIN && self->mode == SIDETOUCH_MODE_TOUCH && self->callbacks.contact)
    self->callbacks.contact (device, self->user_data);
  fpi_ssm_next_state (ssm);
}

static void
send_poll (FpiSsm *ssm, SidetouchAcquire *self)
{
  uint8_t frame[1];

  if (!check_deadline (ssm, self))
    return;
  self->attempts = 0;
  self->transport.ops->bulk_send (self->transport.backend, frame, sidetouch_serial_poll (frame, sizeof frame),
                                  transfer_timeout (ssm, self), cancellable (ssm, self), transfer_done, ssm);
}

static void
receive_status (FpiSsm *ssm, SidetouchAcquire *self)
{
  guint timeout = transfer_timeout (ssm, self);

  if (!timeout)
    {
      check_deadline (ssm, self);
      return;
    }
  self->transport.ops->bulk_receive (self->transport.backend, SIDETOUCH_USB_PACKET, timeout, cancellable (ssm, self),
                                     transfer_done, ssm);
}

static void
run_state (FpiSsm *ssm, FpDevice *device)
{
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);
  uint8_t command[SIDETOUCH_USB_PACKET];
  size_t n;

  switch ((AcqState) fpi_ssm_get_cur_state (ssm))
    {
    case ACQ_RUNTIME:
      if (self->mode != SIDETOUCH_MODE_TOUCH)
        {
          fpi_ssm_next_state (ssm);
          return;
        }
      exchange (ssm, self, sidetouch_cmd_info_a, sizeof sidetouch_cmd_info_a);
      return;

    case ACQ_SETUP:
      if (self->setup_index == 0 && self->mode == SIDETOUCH_MODE_UNTOUCHED)
        {
          /* untouched mode cannot be interrupted: its boundary is here */
          self->past_boundary = TRUE;
          self->deadline = g_get_monotonic_time () + (gint64) SIDETOUCH_UNTOUCHED_DEADLINE_MS * 1000;
          self->expiry = g_cancellable_new ();
          self->expiry_timer = fpi_device_add_timeout (device, SIDETOUCH_UNTOUCHED_DEADLINE_MS, expire, self, NULL);
        }
      switch (self->setup_index++)
        {
        case 0:
          n = sidetouch_cmd_set_param ('F', 0x19, command, sizeof command);
          break;

        case 1:
          n = sidetouch_cmd_set_param ('E', self->mode == SIDETOUCH_MODE_TOUCH ? SIDETOUCH_TOUCH_TIMEOUT
                                                                                : SIDETOUCH_UNTOUCHED_TIMEOUT,
                                       command, sizeof command);
          break;

        default:
          memcpy (command, self->mode == SIDETOUCH_MODE_TOUCH ? sidetouch_cmd_touch : sidetouch_cmd_untouched, 2);
          n = 2;
          break;
        }
      exchange (ssm, self, command, n);
      return;

    case ACQ_ARMED:
      if (self->mode == SIDETOUCH_MODE_TOUCH && self->callbacks.armed)
        self->callbacks.armed (device, self->user_data);
      fpi_ssm_next_state (ssm);
      return;

    case ACQ_WAIT_POLL_OUT:
    case ACQ_CANCEL_POLL_OUT:
      send_poll (ssm, self);
      return;

    case ACQ_WAIT_POLL_IN:
    case ACQ_CANCEL_ACK:
    case ACQ_CANCEL_POLL_IN:
      receive_status (ssm, self);
      return;

    case ACQ_WAIT_DELAY:
    case ACQ_CANCEL_DELAY:
      if (!check_deadline (ssm, self))
        return;
      self->transport.ops->wait (self->transport.backend,
                                 fpi_ssm_get_cur_state (ssm) == ACQ_WAIT_DELAY ? WAIT_DELAY_MS : CANCEL_DELAY_MS,
                                 cancellable (ssm, self), transfer_done, ssm);
      return;

    case ACQ_DRAIN:
    case ACQ_CANCEL_DRAIN:
      if (!check_deadline (ssm, self))
        return;
      sidetouch_serial_read_async (device, &self->transport, transfer_timeout (ssm, self), cancellable (ssm, self),
                                   drain_done, ssm);
      return;

    case ACQ_RESULT:
    case ACQ_CANCEL_STATUS:
      exchange (ssm, self, sidetouch_cmd_status, sizeof sidetouch_cmd_status);
      return;

    case ACQ_CLEAR:
    case ACQ_CANCEL_CLEAR:
      exchange (ssm, self, sidetouch_cmd_clear, sizeof sidetouch_cmd_clear);
      return;

    case ACQ_DATA_READY:
      exchange (ssm, self, sidetouch_cmd_ready, sizeof sidetouch_cmd_ready);
      return;

    case ACQ_CHUNK:
      {
        size_t offset = 0, length = 0;

        if (!sidetouch_chunks_next (&self->chunks, &offset, &length))
          {
            fpi_ssm_mark_failed (ssm, proto_error (ssm, "no range left to read"));
            return;
          }
        n = sidetouch_cmd_range ((uint16_t) offset, (uint16_t) length, command, sizeof command);
        exchange (ssm, self, command, n);
        return;
      }

    case ACQ_DECODE:
      /* the deadline holds for a success too */
      if (deadline_passed (self))
        {
          fpi_ssm_mark_failed (ssm, deadline_error ());
          return;
        }
      if (sidetouch_format4_decode (self->raw, sizeof self->raw, self->samples) != SIDETOUCH_FORMAT4_OK)
        {
          fpi_ssm_mark_failed (ssm, proto_error (ssm, "the stream does not decode"));
          return;
        }
      fpi_ssm_mark_completed (ssm);
      return;

    case ACQ_CANCEL_SEND:
      {
        guint8 frame[SIDETOUCH_USB_PACKET];
        size_t frame_length = sidetouch_eagle_request ((*self->next_sequence)++, NULL, 0, frame, sizeof frame);

        self->attempts = 0;
        self->transport.ops->bulk_send (self->transport.backend, frame, frame_length, self->timeout_ms, NULL,
                                        transfer_done, ssm);
        return;
      }

    case ACQ_NUM_STATES:
    default:
      fpi_ssm_mark_failed (ssm, proto_error (ssm, "unreachable state"));
      return;
    }
}

static void
completed (FpiSsm *ssm, FpDevice *device, GError *error)
{
  SidetouchAcquire *self = fpi_ssm_get_data (ssm);
  SidetouchReaderState reader = SIDETOUCH_READER_KNOWN;
  SidetouchAcquisition result = { 0 };

  if (error && !self->refused &&
      (self->past_boundary || (self->in_cancel_sequence && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))))
    {
      /* past the boundary, or a failed cancellation sequence: the reader's
       * state is not established, so it needs a reconnect */
      GError *wrapped = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                  "SideTouch reader state unknown; if the next attempt fails, "
                                                  "reconnect the reader (%s)", error->message);

      g_error_free (error);
      error = wrapped;
      reader = SIDETOUCH_READER_UNKNOWN;
    }
  if (!error)
    {
      result.samples = self->samples;
      result.status = self->status;
      result.settings = self->mode == SIDETOUCH_MODE_TOUCH ? self->settings : NULL;
    }
  self->callbacks.done (device, error ? NULL : &result, error, reader, self->user_data);
}

SidetouchAcquire *
sidetouch_acquire_start (FpDevice *device, const SidetouchTransport *transport, guint16 *next_sequence,
                         SidetouchMode mode, guint timeout_ms, const SidetouchAcquireCallbacks *callbacks,
                         gpointer user_data)
{
  SidetouchAcquire *self;
  FpiSsm *ssm;

  g_return_val_if_fail (FP_IS_DEVICE (device), NULL);
  g_return_val_if_fail (transport && transport->ops && next_sequence, NULL);
  g_return_val_if_fail (callbacks && callbacks->done, NULL);

  self = g_new0 (SidetouchAcquire, 1);
  self->transport = *transport;
  self->next_sequence = next_sequence;
  self->mode = mode;
  self->timeout_ms = timeout_ms;
  self->work = g_cancellable_new ();
  self->callbacks = *callbacks;
  self->user_data = user_data;
  sidetouch_chunks_init (&self->chunks, self->raw);

  ssm = fpi_ssm_new (device, run_state, ACQ_NUM_STATES);
  fpi_ssm_set_data (ssm, self, (GDestroyNotify) acquire_free);
  fpi_ssm_silence_debug (ssm);
  fpi_ssm_start (ssm, completed);
  return self;
}

void
sidetouch_acquire_cancel (SidetouchAcquire *self)
{
  if (!self || self->cancel_requested)
    return;
  self->cancel_requested = TRUE;
  if (!self->past_boundary)
    g_cancellable_cancel (self->work);
}
