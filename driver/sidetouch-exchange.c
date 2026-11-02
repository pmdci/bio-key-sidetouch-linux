/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * SideTouch Eagle exchange over the serial bridge. See sidetouch-exchange.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch-exchange.h"
#include "sidetouch-proto.h"
#include "sidetouch-wipe.h"

#include <string.h>

G_DEFINE_QUARK (sidetouch-exchange-error-quark, sidetouch_exchange_error)

typedef enum {
  EXCHANGE_PREFLIGHT_OUT,
  EXCHANGE_PREFLIGHT_IN,
  EXCHANGE_REQUEST_OUT,
  EXCHANGE_ACK_IN,
  EXCHANGE_POLL_OUT,
  EXCHANGE_POLL_IN,
  EXCHANGE_LENGTH_OUT,
  EXCHANGE_LENGTH_IN,
  EXCHANGE_DATA_OUT,
  EXCHANGE_DATA_IN,
  EXCHANGE_VALIDATE,
  EXCHANGE_NUM_STATES,
} ExchangeState;

typedef struct
{
  SidetouchTransport    transport;
  GCancellable         *cancellable;
  guint                 timeout_ms;
  gboolean              bare;        /* serial read only */
  guint16               sequence;
  guint8                command[2];  /* first payload bytes, for diagnostics */
  guint8                request[SIDETOUCH_USB_PACKET];
  gsize                 request_length;
  guint                 preflight;
  unsigned              attempts;
  uint16_t              announced;
  SidetouchSerialRead   read;
  const guint8         *result;
  gsize                 result_length;
  SidetouchExchangeDone done;
  gpointer              user_data;
} Exchange;

static void
exchange_free (Exchange *self)
{
  g_clear_object (&self->cancellable);
  /* the read may hold sensor data from a range read */
  sidetouch_wipe (&self->read, sizeof self->read);
  g_free (self);
}

static void
exchange_fail (FpiSsm *ssm, const char *what)
{
  Exchange *self = fpi_ssm_get_data (ssm);

  fpi_ssm_mark_failed (ssm,
                       fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                 "SideTouch %s (command %02x %02x, sequence %u, state %d)",
                                                 what, self->command[0], self->command[1],
                                                 self->sequence, fpi_ssm_get_cur_state (ssm)));
}

static void
send_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;

  (void) data;
  (void) length;
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

/* A status poll, preflight poll or write acknowledgement. TRUE when a state
 * byte arrived; otherwise the machine has already moved on. */
static gboolean
status_ready (FpiSsm *ssm, Exchange *self, const guint8 *data, gsize length)
{
  switch (sidetouch_poll_progress (data, length, &self->attempts, NULL))
    {
    case SIDETOUCH_POLL_READY:
      return TRUE;

    case SIDETOUCH_POLL_RETRY:
      fpi_ssm_jump_to_state (ssm, fpi_ssm_get_cur_state (ssm));
      return FALSE;

    case SIDETOUCH_POLL_EXHAUSTED:
      exchange_fail (ssm, "serial status never arrived");
      return FALSE;

    case SIDETOUCH_POLL_INVALID:
    default:
      exchange_fail (ssm, "invalid serial status reply");
      return FALSE;
    }
}

static void
receive_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  Exchange *self = fpi_ssm_get_data (ssm);
  int state = fpi_ssm_get_cur_state (ssm);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  switch ((ExchangeState) state)
    {
    case EXCHANGE_PREFLIGHT_IN:
      if (!status_ready (ssm, self, data, length))
        return;
      if (++self->preflight < SIDETOUCH_PREFLIGHT_POLLS)
        fpi_ssm_jump_to_state (ssm, EXCHANGE_PREFLIGHT_OUT);
      else
        fpi_ssm_next_state (ssm);
      return;

    case EXCHANGE_ACK_IN:
    case EXCHANGE_POLL_IN:
      if (status_ready (ssm, self, data, length))
        fpi_ssm_next_state (ssm);
      return;

    case EXCHANGE_LENGTH_IN:
    case EXCHANGE_DATA_IN:
      switch (sidetouch_serial_read_feed (&self->read, data, length))
        {
        case SIDETOUCH_COLLECT_MORE:
          fpi_ssm_jump_to_state (ssm, state);
          return;

        case SIDETOUCH_COLLECT_DONE:
          if (state == EXCHANGE_LENGTH_IN &&
              sidetouch_serial_length_reply (self->read.collector.bytes, self->read.collector.length,
                                             &self->announced) != SIDETOUCH_PROTO_OK)
            {
              exchange_fail (ssm, "invalid serial length reply");
              return;
            }
          fpi_ssm_next_state (ssm);
          return;

        case SIDETOUCH_COLLECT_ERROR:
        default:
          exchange_fail (ssm, "malformed or oversized serial read");
          return;
        }

    case EXCHANGE_PREFLIGHT_OUT:
    case EXCHANGE_REQUEST_OUT:
    case EXCHANGE_POLL_OUT:
    case EXCHANGE_LENGTH_OUT:
    case EXCHANGE_DATA_OUT:
    case EXCHANGE_VALIDATE:
    case EXCHANGE_NUM_STATES:
    default:
      exchange_fail (ssm, "unexpected receive completion");
      return;
    }
}

static void
send_bytes (FpiSsm *ssm, Exchange *self, const guint8 *data, gsize length)
{
  self->attempts = 0;
  self->transport.ops->bulk_send (self->transport.backend, data, length, self->timeout_ms,
                                  self->cancellable, send_done, ssm);
}

static void
receive_packet (FpiSsm *ssm, Exchange *self, guint timeout_ms)
{
  self->transport.ops->bulk_receive (self->transport.backend, SIDETOUCH_USB_PACKET, timeout_ms,
                                     self->cancellable, receive_done, ssm);
}

/* One packet of a logical serial read, within the read's single deadline. */
static void
collect (FpiSsm *ssm, Exchange *self, size_t expected)
{
  guint remaining;

  if (!sidetouch_serial_read_begin (&self->read, expected, self->timeout_ms))
    {
      exchange_fail (ssm, "serial read does not fit");
      return;
    }
  remaining = sidetouch_serial_read_remaining (&self->read);
  if (!remaining)
    {
      fpi_ssm_mark_failed (ssm, sidetouch_serial_read_timeout_error (&self->read));
      return;
    }
  receive_packet (ssm, self, remaining);
}

static void
validate (FpiSsm *ssm, Exchange *self)
{
  const guint8 *bytes = self->read.collector.bytes;
  gsize length = self->read.collector.length;
  const uint8_t *payload;
  size_t payload_length;

  if (length < 3 || bytes[2] != 0x62)
    {
      exchange_fail (ssm, "invalid serial envelope");
      return;
    }
  if (self->bare)
    {
      self->result = bytes;
      self->result_length = length;
      fpi_ssm_mark_completed (ssm);
      return;
    }
  if (sidetouch_recorded_empty_response (bytes, length))
    {
      fpi_ssm_mark_failed (ssm, g_error_new (SIDETOUCH_EXCHANGE_ERROR,
                                             SIDETOUCH_EXCHANGE_ERROR_EMPTY_RESPONSE,
                                             "SideTouch returned an empty response (sequence %u)",
                                             self->sequence));
      return;
    }
  if (sidetouch_eagle_response (bytes, length, self->sequence, &payload, &payload_length) !=
      SIDETOUCH_PROTO_OK)
    {
      exchange_fail (ssm, "invalid Eagle response");
      return;
    }
  self->result = payload;
  self->result_length = payload_length;
  fpi_ssm_mark_completed (ssm);
}

static void
run_state (FpiSsm *ssm, FpDevice *device)
{
  Exchange *self = fpi_ssm_get_data (ssm);
  uint8_t frame[SIDETOUCH_USB_PACKET];
  size_t frame_length;

  (void) device;
  switch ((ExchangeState) fpi_ssm_get_cur_state (ssm))
    {
    case EXCHANGE_PREFLIGHT_OUT:
      if (self->bare)
        {
          fpi_ssm_jump_to_state (ssm, EXCHANGE_LENGTH_OUT);
          return;
        }
      /* fall through */
    case EXCHANGE_POLL_OUT:
      frame_length = sidetouch_serial_poll (frame, sizeof frame);
      send_bytes (ssm, self, frame, frame_length);
      return;

    case EXCHANGE_REQUEST_OUT:
      send_bytes (ssm, self, self->request, self->request_length);
      return;

    case EXCHANGE_PREFLIGHT_IN:
    case EXCHANGE_ACK_IN:
    case EXCHANGE_POLL_IN:
      receive_packet (ssm, self, self->timeout_ms);
      return;

    case EXCHANGE_LENGTH_OUT:
      frame_length = sidetouch_serial_length_request (frame, sizeof frame);
      send_bytes (ssm, self, frame, frame_length);
      return;

    case EXCHANGE_LENGTH_IN:
      collect (ssm, self, SIDETOUCH_SERIAL_LENGTH_REPLY);
      return;

    case EXCHANGE_DATA_OUT:
      frame_length = sidetouch_serial_data_request (self->announced, frame, sizeof frame);
      if (!frame_length)
        {
          exchange_fail (ssm, "announced length does not fit");
          return;
        }
      send_bytes (ssm, self, frame, frame_length);
      return;

    case EXCHANGE_DATA_IN:
      collect (ssm, self, (size_t) self->announced + 3);
      return;

    case EXCHANGE_VALIDATE:
      validate (ssm, self);
      return;

    case EXCHANGE_NUM_STATES:
    default:
      exchange_fail (ssm, "unreachable state");
      return;
    }
}

static void
completed (FpiSsm *ssm, FpDevice *device, GError *error)
{
  Exchange *self = fpi_ssm_get_data (ssm);

  if (!self->done)
    {
      g_clear_error (&error);
      return;
    }
  self->done (device, error ? NULL : self->result, error ? 0 : self->result_length, error,
              self->user_data);
}

static Exchange *
exchange_new (const SidetouchTransport *transport, guint timeout_ms, GCancellable *cancellable,
              SidetouchExchangeDone done, gpointer user_data)
{
  Exchange *self = g_new0 (Exchange, 1);

  self->transport = *transport;
  self->timeout_ms = timeout_ms;
  self->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  self->done = done;
  self->user_data = user_data;
  return self;
}

static void
start (FpDevice *device, Exchange *self)
{
  FpiSsm *ssm = fpi_ssm_new (device, run_state, EXCHANGE_NUM_STATES);

  fpi_ssm_set_data (ssm, self, (GDestroyNotify) exchange_free);
  fpi_ssm_silence_debug (ssm);
  fpi_ssm_start (ssm, completed);
}

void
sidetouch_exchange_async (FpDevice *device, const SidetouchTransport *transport, guint16 sequence,
                          const guint8 *payload, gsize payload_length, guint timeout_ms,
                          GCancellable *cancellable, SidetouchExchangeDone done, gpointer user_data)
{
  Exchange *self;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (transport && transport->ops);
  g_return_if_fail (payload || payload_length == 0);

  self = exchange_new (transport, timeout_ms, cancellable, done, user_data);
  self->sequence = sequence;
  if (payload_length > 0)
    self->command[0] = payload[0];
  if (payload_length > 1)
    self->command[1] = payload[1];
  self->request_length = sidetouch_eagle_request (sequence, payload, payload_length, self->request,
                                                  sizeof self->request);
  if (!self->request_length)
    {
      GError *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                "SideTouch request does not fit one packet");

      if (done)
        done (device, NULL, 0, error, user_data);
      else
        g_error_free (error);
      exchange_free (self);
      return;
    }
  start (device, self);
}

void
sidetouch_serial_read_async (FpDevice *device, const SidetouchTransport *transport, guint timeout_ms,
                             GCancellable *cancellable, SidetouchExchangeDone done, gpointer user_data)
{
  Exchange *self;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (transport && transport->ops);

  self = exchange_new (transport, timeout_ms, cancellable, done, user_data);
  self->bare = TRUE;
  start (device, self);
}

gboolean
sidetouch_serial_read_begin (SidetouchSerialRead *read, gsize expected, guint timeout_ms)
{
  if (read->active)
    return read->collector.expected == expected;
  if (!sidetouch_collector_init (&read->collector, expected))
    return FALSE;
  read->active = TRUE;
  read->deadline = g_get_monotonic_time () + (gint64) MAX (timeout_ms, 1u) * 1000;
  return TRUE;
}

guint
sidetouch_serial_read_remaining (const SidetouchSerialRead *read)
{
  gint64 left = read->deadline - g_get_monotonic_time ();

  return left > 0 ? (guint) MAX ((left + 999) / 1000, 1) : 0;
}

SidetouchCollectResult
sidetouch_serial_read_feed (SidetouchSerialRead *read, const guint8 *data, gsize length)
{
  SidetouchCollectResult r = sidetouch_collector_feed (&read->collector, data, length);

  if (r != SIDETOUCH_COLLECT_MORE)
    read->active = FALSE;
  return r;
}

GError *
sidetouch_serial_read_timeout_error (const SidetouchSerialRead *read)
{
  return g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                      "SideTouch serial read timed out (%" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes)",
                      read->collector.length, read->collector.expected);
}
