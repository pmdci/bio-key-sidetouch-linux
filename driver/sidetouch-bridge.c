/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * SideTouch serial-bridge initialisation. See sidetouch-bridge.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch-bridge.h"
#include "sidetouch-exchange.h"
#include "sidetouch-proto.h"

#include <string.h>

#define SWEEP_PASSES 20
#define SWEEP_MAX_FIELD 512
#define SWEEP_DELAY_MS 5
#define SWEEPS 3
#define RECOVERY_SETTLE_MS 200
#define RECOVERY_POLL_TIMEOUT_MS 2000
#define RECOVERY_GAP_MS 100

typedef enum {
  STEP_CONTROL,   /* vendor control OUT */
  STEP_SEND,      /* a fixed bulk frame */
  STEP_POLL,      /* poll request, then a status read */
  STEP_DRAIN,     /* one bulk IN of at least two bytes, contents unused */
  STEP_EXPECT,    /* a serial read of the given length, matched as below */
} StepKind;

typedef enum {
  MATCH_EXACT,       /* every serial byte */
  MATCH_WRITE_ACK,   /* only the first serial byte, 0x62 */
  MATCH_FINAL,       /* every serial byte but the last, which is 0x80 or 0x00 */
} Match;

typedef struct
{
  StepKind      kind;
  guint8        request;
  guint16       value;
  guint16       index;
  const guint8 *data;
  gsize         length;
  Match         match;
} Step;

#define CONTROL(r, v, i) { .kind = STEP_CONTROL, .request = (r), .value = (v), .index = (i) }
#define SEND(bytes) { .kind = STEP_SEND, .data = (bytes), .length = sizeof (bytes) }
#define EXPECT(bytes, m) { .kind = STEP_EXPECT, .data = (bytes), .length = sizeof (bytes), .match = (m) }
#define POLL { .kind = STEP_POLL }
#define DRAIN { .kind = STEP_DRAIN }

/* The reduced control sequence established on Linux. */
static const Step control_steps[] = {
  CONTROL (0x00, 0x0000, 0x0000), CONTROL (0x00, 0x0000, 0x0001), CONTROL (0x00, 0x0000, 0x0002), DRAIN,
  CONTROL (0x00, 0x0000, 0x0000), CONTROL (0x06, 0x0000, 0x0000), CONTROL (0x07, 0x0000, 0x0000), DRAIN,
  CONTROL (0x09, 0x0001, 0x0000), CONTROL (0x0b, 0x0000, 0x0000), DRAIN,
  CONTROL (0x0b, 0x0200, 0x0000), DRAIN,
  CONTROL (0x00, 0x0000, 0x0000),
};

static const guint8 setup_1[] = { 0x8a };
static const guint8 setup_2[] = { 0x86, 0x03, 0x00 };
static const guint8 setup_3[] = { 0x9e, 0x00, 0x40 };
static const guint8 setup_4[] = { 0x80, 0x00, 0x03, 0x82, 0x42, 0x42 };
static const guint8 setup_5[] = { 0x80, 0x42, 0x03, 0x82, 0x42, 0x42 };

static const Step preamble_steps[] = {
  SEND (setup_1), SEND (setup_2), CONTROL (0x00, 0x0000, 0x0000),
  SEND (setup_3), SEND (setup_4), SEND (setup_5),
  POLL,
};

static const guint8 seven_byte_setup[] = {
  0x82, 0x40, 0x42, 0x31, 0x07, 0x00, 0x00,
  0x04, 0x00, 0x05, 0x00, 0x2f, 0x00, 0x00,
  0x82, 0x42, 0x42,
};
static const guint8 setup_ack[] = { 0x32, 0x60, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62 };
static const guint8 read_2[] = { 0x82, 0x40, 0x42, 0x31, 0x02, 0x00, 0x01, 0xff, 0xff, 0x82, 0x42, 0x42 };
static const guint8 reply_length_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00 };
static const guint8 read_5[] = {
  0x82, 0x40, 0x42, 0x31, 0x05, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0x82, 0x42, 0x42,
};
static const guint8 reply_5[] = { 0x32, 0x60, 0x62, 0x05, 0x00, 0x1f, 0x00, 0x80 };

static const Step final_steps[] = {
  SEND (seven_byte_setup), EXPECT (setup_ack, MATCH_WRITE_ACK),
  POLL,
  SEND (read_2), EXPECT (reply_length_5, MATCH_EXACT),
  SEND (read_5), EXPECT (reply_5, MATCH_FINAL),
};

typedef enum { PHASE_CONTROL, PHASE_PREAMBLE, PHASE_FINAL } Phase;

typedef enum {
  BRIDGE_STEP,
  BRIDGE_STEP_REPLY,
  BRIDGE_SWEEP_POLL_OUT,
  BRIDGE_SWEEP_POLL_IN,
  BRIDGE_SWEEP_FIELD_OUT,
  BRIDGE_SWEEP_FIELD_IN,
  BRIDGE_SWEEP_PAYLOAD_OUT,
  BRIDGE_SWEEP_PAYLOAD_IN,
  BRIDGE_SWEEP_DELAY,
  BRIDGE_RECOVER,
  BRIDGE_RECOVER_SETTLE,
  BRIDGE_RECOVER_POLL_OUT,
  BRIDGE_RECOVER_POLL_IN,
  BRIDGE_RECOVER_GAP,
  BRIDGE_NUM_STATES,
} BridgeState;

typedef struct
{
  SidetouchTransport  transport;
  GCancellable       *cancellable;
  guint               timeout_ms;
  Phase               phase;
  const Step         *steps;
  gsize               n_steps;
  gsize               step;
  unsigned            attempts;   /* reads of the current status reply */
  unsigned            passes;     /* sweep passes */
  guint               sweep;      /* 0, 1, 2 */
  uint16_t            field;
  SidetouchSerialRead read;
  SidetouchBridgeDone done;
  gpointer            user_data;
} Bridge;

static void
bridge_free (Bridge *self)
{
  g_clear_object (&self->cancellable);
  g_free (self);
}

static GError *
proto_error (FpiSsm *ssm, const char *what)
{
  Bridge *self = fpi_ssm_get_data (ssm);

  return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                   "SideTouch bridge initialisation: %s (phase %d, step %" G_GSIZE_FORMAT
                                   ", sweep %u, state %d)",
                                   what, self->phase, self->step, self->sweep, fpi_ssm_get_cur_state (ssm));
}

/* The reply that differed, for diagnosis: a short bridge status reply, never
 * sensor data. */
static void
log_reply (const guint8 *bytes, gsize length)
{
  g_autoptr(GString) hex = g_string_new (NULL);

  for (gsize i = 0; i < length && i < 32; i++)
    g_string_append_printf (hex, i ? " %02x" : "%02x", bytes[i]);
  fp_dbg ("SideTouch bridge reply (%" G_GSIZE_FORMAT " bytes): %s%s", length, hex->str, length > 32 ? " ..." : "");
}

static void
load_phase (Bridge *self, Phase phase)
{
  self->phase = phase;
  self->step = 0;
  switch (phase)
    {
    case PHASE_CONTROL:
      self->steps = control_steps;
      self->n_steps = G_N_ELEMENTS (control_steps);
      break;

    case PHASE_PREAMBLE:
      self->steps = preamble_steps;
      self->n_steps = G_N_ELEMENTS (preamble_steps);
      break;

    case PHASE_FINAL:
      self->steps = final_steps;
      self->n_steps = G_N_ELEMENTS (final_steps);
      break;
    }
}

static void
start_sweep (FpiSsm *ssm, Bridge *self)
{
  self->passes = 0;
  self->read.active = FALSE;
  fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_POLL_OUT);
}

static void
advance_step (FpiSsm *ssm, Bridge *self)
{
  if (++self->step < self->n_steps)
    {
      fpi_ssm_jump_to_state (ssm, BRIDGE_STEP);
      return;
    }
  switch (self->phase)
    {
    case PHASE_CONTROL:
      load_phase (self, PHASE_PREAMBLE);
      fpi_ssm_jump_to_state (ssm, BRIDGE_STEP);
      return;

    case PHASE_PREAMBLE:
      start_sweep (ssm, self);
      return;

    case PHASE_FINAL:
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

/* A failed sweep: recover and sweep again, or give up after the third. A
 * cancellation always ends the transaction. */
static void
sweep_failed (FpiSsm *ssm, GError *error)
{
  Bridge *self = fpi_ssm_get_data (ssm);

  self->read.active = FALSE;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->sweep + 1 >= SWEEPS)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_dbg ("SideTouch bridge sweep %u failed (%s); recovering", self->sweep, error->message);
  g_error_free (error);
  fpi_ssm_jump_to_state (ssm, self->sweep == 0 ? BRIDGE_RECOVER : BRIDGE_RECOVER_GAP);
}

static gboolean
in_sweep (int state)
{
  return state >= BRIDGE_SWEEP_POLL_OUT && state <= BRIDGE_SWEEP_DELAY;
}

static void
fail (FpiSsm *ssm, GError *error)
{
  if (in_sweep (fpi_ssm_get_cur_state (ssm)))
    sweep_failed (ssm, error);
  else
    fpi_ssm_mark_failed (ssm, error);
}

static void
done_next (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  Bridge *self = fpi_ssm_get_data (ssm);

  (void) data;
  (void) length;
  if (error)
    {
      fail (ssm, error);
      return;
    }
  switch ((BridgeState) fpi_ssm_get_cur_state (ssm))
    {
    case BRIDGE_STEP:
      if (self->steps[self->step].kind == STEP_POLL)
        fpi_ssm_jump_to_state (ssm, BRIDGE_STEP_REPLY);
      else
        advance_step (ssm, self);
      return;

    case BRIDGE_SWEEP_DELAY:
      fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_POLL_OUT);
      return;

    case BRIDGE_STEP_REPLY:
    case BRIDGE_SWEEP_POLL_OUT:
    case BRIDGE_SWEEP_POLL_IN:
    case BRIDGE_SWEEP_FIELD_OUT:
    case BRIDGE_SWEEP_FIELD_IN:
    case BRIDGE_SWEEP_PAYLOAD_OUT:
    case BRIDGE_SWEEP_PAYLOAD_IN:
    case BRIDGE_RECOVER:
    case BRIDGE_RECOVER_SETTLE:
    case BRIDGE_RECOVER_POLL_OUT:
    case BRIDGE_RECOVER_POLL_IN:
    case BRIDGE_RECOVER_GAP:
    case BRIDGE_NUM_STATES:
    default:
      fpi_ssm_next_state (ssm);
      return;
    }
}

/* A status read: TRUE with the state byte, FALSE once handled otherwise. */
static gboolean
status (FpiSsm *ssm, Bridge *self, const guint8 *data, gsize length, uint8_t *state)
{
  switch (sidetouch_poll_progress (data, length, &self->attempts, state))
    {
    case SIDETOUCH_POLL_READY:
      return TRUE;

    case SIDETOUCH_POLL_RETRY:
      fpi_ssm_jump_to_state (ssm, fpi_ssm_get_cur_state (ssm));
      return FALSE;

    case SIDETOUCH_POLL_EXHAUSTED:
      fail (ssm, proto_error (ssm, "serial status never arrived"));
      return FALSE;

    case SIDETOUCH_POLL_INVALID:
    default:
      fail (ssm, proto_error (ssm, "invalid serial status reply"));
      return FALSE;
    }
}

/* One packet of a serial read; TRUE when the read is complete. */
static gboolean
collected (FpiSsm *ssm, Bridge *self, const guint8 *data, gsize length)
{
  switch (sidetouch_serial_read_feed (&self->read, data, length))
    {
    case SIDETOUCH_COLLECT_MORE:
      fpi_ssm_jump_to_state (ssm, fpi_ssm_get_cur_state (ssm));
      return FALSE;

    case SIDETOUCH_COLLECT_DONE:
      return TRUE;

    case SIDETOUCH_COLLECT_ERROR:
    default:
      fail (ssm, proto_error (ssm, "malformed or oversized serial read"));
      return FALSE;
    }
}

static gboolean
reply_matches (const Step *step, const guint8 *data, gsize length)
{
  if (length != step->length || length < 3 || data[2] != 0x62)
    return FALSE;
  switch (step->match)
    {
    case MATCH_WRITE_ACK:
      return TRUE;

    case MATCH_FINAL:
      return memcmp (data + 2, step->data + 2, length - 3) == 0 &&
             (data[length - 1] == 0x80 || data[length - 1] == 0x00);

    case MATCH_EXACT:
    default:
      return memcmp (data + 2, step->data + 2, length - 2) == 0;
    }
}

static void
received (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  Bridge *self = fpi_ssm_get_data (ssm);
  uint8_t state = 0;

  if (error)
    {
      fail (ssm, error);
      return;
    }

  switch ((BridgeState) fpi_ssm_get_cur_state (ssm))
    {
    case BRIDGE_STEP_REPLY:
      {
        const Step *step = &self->steps[self->step];

        if (step->kind == STEP_POLL)
          {
            if (status (ssm, self, data, length, NULL))
              advance_step (ssm, self);
            return;
          }
        if (step->kind == STEP_DRAIN)
          {
            if (!data || length < 2)
              fail (ssm, proto_error (ssm, "short drain"));
            else
              advance_step (ssm, self);
            return;
          }
        if (!collected (ssm, self, data, length))
          return;
        if (!reply_matches (step, self->read.collector.bytes, self->read.collector.length))
          {
            log_reply (self->read.collector.bytes, self->read.collector.length);
            fail (ssm, proto_error (ssm, "reply differs from the established sequence"));
          }
        else
          advance_step (ssm, self);
        return;
      }

    case BRIDGE_SWEEP_POLL_IN:
      if (!status (ssm, self, data, length, &state))
        return;
      if (length != 3)
        {
          fail (ssm, proto_error (ssm, "invalid sweep status length"));
          return;
        }
      if ((state & 0x20) || ++self->passes >= SWEEP_PASSES)
        {
          if (self->sweep + 1 >= SWEEPS)
            {
              fpi_ssm_mark_failed (ssm, proto_error (ssm, "the terminal third sweep cannot complete initialisation"));
              return;
            }
          load_phase (self, PHASE_FINAL);
          fpi_ssm_jump_to_state (ssm, BRIDGE_STEP);
          return;
        }
      fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_FIELD_OUT);
      return;

    case BRIDGE_SWEEP_FIELD_IN:
      if (!collected (ssm, self, data, length))
        return;
      {
        const guint8 *r = self->read.collector.bytes;

        if (r[2] != 0x62)
          {
            fail (ssm, proto_error (ssm, "invalid two-byte field reply"));
            return;
          }
        self->field = (uint16_t) (r[3] | (r[4] << 8));
      }
      if (self->field <= 2)
        fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_DELAY);
      else if (self->field <= SWEEP_MAX_FIELD)
        fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_PAYLOAD_OUT);
      else
        fail (ssm, proto_error (ssm, "two-byte field above 512"));
      return;

    case BRIDGE_SWEEP_PAYLOAD_IN:
      if (!collected (ssm, self, data, length))
        return;
      if (self->read.collector.bytes[2] != 0x62)
        fail (ssm, proto_error (ssm, "invalid sweep payload"));
      else
        fpi_ssm_jump_to_state (ssm, BRIDGE_SWEEP_DELAY);
      return;

    case BRIDGE_RECOVER_POLL_IN:
      /* any outcome is accepted; only a transport-only reply is read again */
      if (sidetouch_poll_progress (data, length, &self->attempts, NULL) == SIDETOUCH_POLL_RETRY)
        fpi_ssm_jump_to_state (ssm, BRIDGE_RECOVER_POLL_IN);
      else
        fpi_ssm_jump_to_state (ssm, BRIDGE_RECOVER_GAP);
      return;

    case BRIDGE_STEP:
    case BRIDGE_SWEEP_POLL_OUT:
    case BRIDGE_SWEEP_FIELD_OUT:
    case BRIDGE_SWEEP_PAYLOAD_OUT:
    case BRIDGE_SWEEP_DELAY:
    case BRIDGE_RECOVER:
    case BRIDGE_RECOVER_SETTLE:
    case BRIDGE_RECOVER_POLL_OUT:
    case BRIDGE_RECOVER_GAP:
    case BRIDGE_NUM_STATES:
    default:
      fpi_ssm_mark_failed (ssm, proto_error (ssm, "unexpected receive completion"));
      return;
    }
}

/* The IN recovery and the readiness poll tolerate a transport failure: they
 * are themselves recovery, and the sweep that follows decides. */
static void
recovery_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  int state = fpi_ssm_get_cur_state (ssm);

  (void) data;
  (void) length;
  if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (error)
    {
      fp_dbg ("SideTouch bridge recovery step %d failed: %s", state, error->message);
      g_error_free (error);
      fpi_ssm_jump_to_state (ssm, BRIDGE_RECOVER_GAP);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
recovery_received (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  if (error)
    recovery_done (data, length, error, user_data);
  else
    received (data, length, NULL, user_data);
}

static void
gap_done (const guint8 *data, gsize length, GError *error, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  Bridge *self = fpi_ssm_get_data (ssm);

  (void) data;
  (void) length;
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  self->sweep++;
  start_sweep (ssm, self);
}

static void
send_frame (Bridge *self, FpiSsm *ssm, const guint8 *data, gsize length, guint timeout_ms,
            SidetouchTransferDone done)
{
  self->attempts = 0;
  self->transport.ops->bulk_send (self->transport.backend, data, length, timeout_ms, self->cancellable, done, ssm);
}

static void
receive_packet (Bridge *self, FpiSsm *ssm, guint timeout_ms, SidetouchTransferDone done)
{
  self->transport.ops->bulk_receive (self->transport.backend, SIDETOUCH_USB_PACKET, timeout_ms,
                                     self->cancellable, done, ssm);
}

static void
collect (FpiSsm *ssm, Bridge *self, gsize expected)
{
  guint remaining;

  if (!sidetouch_serial_read_begin (&self->read, expected, self->timeout_ms))
    {
      fail (ssm, proto_error (ssm, "serial read does not fit"));
      return;
    }
  remaining = sidetouch_serial_read_remaining (&self->read);
  if (!remaining)
    {
      self->read.active = FALSE;
      fail (ssm, sidetouch_serial_read_timeout_error (&self->read));
      return;
    }
  receive_packet (self, ssm, remaining, received);
}

static void
run_state (FpiSsm *ssm, FpDevice *device)
{
  Bridge *self = fpi_ssm_get_data (ssm);
  const SidetouchTransportOps *ops = self->transport.ops;
  uint8_t frame[SWEEP_MAX_FIELD + 16];
  size_t n;

  (void) device;
  switch ((BridgeState) fpi_ssm_get_cur_state (ssm))
    {
    case BRIDGE_STEP:
      {
        const Step *step = &self->steps[self->step];

        switch (step->kind)
          {
          case STEP_CONTROL:
            ops->control_out (self->transport.backend, step->request, step->value, step->index,
                              self->timeout_ms, self->cancellable, done_next, ssm);
            return;

          case STEP_SEND:
            send_frame (self, ssm, step->data, step->length, self->timeout_ms, done_next);
            return;

          case STEP_POLL:
            n = sidetouch_serial_poll (frame, sizeof frame);
            send_frame (self, ssm, frame, n, self->timeout_ms, done_next);
            return;

          case STEP_DRAIN:
          case STEP_EXPECT:
          default:
            fpi_ssm_jump_to_state (ssm, BRIDGE_STEP_REPLY);
            return;
          }
      }

    case BRIDGE_STEP_REPLY:
      if (self->steps[self->step].kind == STEP_EXPECT)
        collect (ssm, self, self->steps[self->step].length);
      else
        receive_packet (self, ssm, self->timeout_ms, received);
      return;

    case BRIDGE_SWEEP_POLL_OUT:
      n = sidetouch_serial_poll (frame, sizeof frame);
      send_frame (self, ssm, frame, n, self->timeout_ms, done_next);
      return;

    case BRIDGE_SWEEP_POLL_IN:
      receive_packet (self, ssm, self->timeout_ms, received);
      return;

    case BRIDGE_SWEEP_FIELD_OUT:
      n = sidetouch_serial_length_request (frame, sizeof frame);
      send_frame (self, ssm, frame, n, self->timeout_ms, done_next);
      return;

    case BRIDGE_SWEEP_FIELD_IN:
      collect (ssm, self, SIDETOUCH_SERIAL_LENGTH_REPLY);
      return;

    case BRIDGE_SWEEP_PAYLOAD_OUT:
      /* up to 512 bytes, so the request may span two packets */
      n = (size_t) self->field + SIDETOUCH_SERIAL_DATA_OVERHEAD;
      frame[0] = 0x82;
      frame[1] = 0x40;
      frame[2] = 0x42;
      frame[3] = 0x31;
      frame[4] = (uint8_t) self->field;
      frame[5] = (uint8_t) (self->field >> 8);
      frame[6] = 0x01;
      memset (frame + 7, 0xff, self->field);
      frame[7 + self->field] = 0x82;
      frame[8 + self->field] = 0x42;
      frame[9 + self->field] = 0x42;
      send_frame (self, ssm, frame, n, self->timeout_ms, done_next);
      return;

    case BRIDGE_SWEEP_PAYLOAD_IN:
      collect (ssm, self, (gsize) self->field + 3);
      return;

    case BRIDGE_SWEEP_DELAY:
      ops->wait (self->transport.backend, SWEEP_DELAY_MS, self->cancellable, done_next, ssm);
      return;

    case BRIDGE_RECOVER:
      ops->recover_in (self->transport.backend, self->cancellable, recovery_done, ssm);
      return;

    case BRIDGE_RECOVER_SETTLE:
      /* unlike the other recovery steps, a failed settle ends initialisation */
      ops->wait (self->transport.backend, RECOVERY_SETTLE_MS, self->cancellable, done_next, ssm);
      return;

    case BRIDGE_RECOVER_POLL_OUT:
      n = sidetouch_serial_poll (frame, sizeof frame);
      send_frame (self, ssm, frame, n, RECOVERY_POLL_TIMEOUT_MS, recovery_done);
      return;

    case BRIDGE_RECOVER_POLL_IN:
      receive_packet (self, ssm, RECOVERY_POLL_TIMEOUT_MS, recovery_received);
      return;

    case BRIDGE_RECOVER_GAP:
      ops->wait (self->transport.backend, RECOVERY_GAP_MS, self->cancellable, gap_done, ssm);
      return;

    case BRIDGE_NUM_STATES:
    default:
      fpi_ssm_mark_failed (ssm, proto_error (ssm, "unreachable state"));
      return;
    }
}

static void
completed (FpiSsm *ssm, FpDevice *device, GError *error)
{
  Bridge *self = fpi_ssm_get_data (ssm);

  if (self->done)
    self->done (device, error, self->user_data);
  else
    g_clear_error (&error);
}

void
sidetouch_bridge_init_async (FpDevice *device, const SidetouchTransport *transport, guint timeout_ms,
                             GCancellable *cancellable, SidetouchBridgeDone done, gpointer user_data)
{
  Bridge *self;
  FpiSsm *ssm;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (transport && transport->ops);

  self = g_new0 (Bridge, 1);
  self->transport = *transport;
  self->timeout_ms = timeout_ms;
  self->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  self->done = done;
  self->user_data = user_data;
  load_phase (self, PHASE_CONTROL);

  ssm = fpi_ssm_new (device, run_state, BRIDGE_NUM_STATES);
  fpi_ssm_set_data (ssm, self, (GDestroyNotify) bridge_free);
  fpi_ssm_silence_debug (ssm);
  fpi_ssm_start (ssm, completed);
}
