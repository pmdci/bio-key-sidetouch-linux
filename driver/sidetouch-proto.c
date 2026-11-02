/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * BIO-key SideTouch wire protocol: pure framing. See sidetouch-proto.h.
 */
#include "sidetouch-proto.h"

#include <string.h>

#define REQUEST_MAGIC 0xabba
#define RESPONSE_MAGIC 0xf0ca
#define SERIAL_READ 0x62

const uint8_t sidetouch_cmd_info_b[2] = { 0x01, 0x42 };
const uint8_t sidetouch_cmd_info_a[2] = { 0x01, 0x41 };
const uint8_t sidetouch_cmd_status[2] = { 0x01, 0x43 };
const uint8_t sidetouch_cmd_clear[5] = { 0x02, 0x43, 0xff, 0xff, 0x00 };
const uint8_t sidetouch_cmd_ready[2] = { 0x01, 0x49 };
const uint8_t sidetouch_cmd_touch[2] = { 0x08, 0x07 };
const uint8_t sidetouch_cmd_untouched[2] = { 0x08, 0x05 };

static uint16_t
get_u16 (const uint8_t *p)
{
  return (uint16_t) (p[0] | (p[1] << 8));
}

static void
put_u16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t) v;
  p[1] = (uint8_t) (v >> 8);
}

/* CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xffff, no reflection, no final XOR. */
static uint16_t
crc16_update (uint16_t crc, const uint8_t *data, size_t length)
{
  for (size_t i = 0; i < length; i++)
    {
      crc ^= (uint16_t) (data[i] << 8);
      for (int bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
    }
  return crc;
}

uint16_t
sidetouch_crc16 (const uint8_t *data, size_t length)
{
  return crc16_update (0xffff, data, length);
}

/* "82 40 42 31 <len> <dir>" ... "82 42 42" around len bytes written at out + 7. */
static size_t
serial_frame (uint8_t dir, size_t len, uint8_t *out, size_t capacity)
{
  if (len > UINT16_MAX || len + SIDETOUCH_SERIAL_DATA_OVERHEAD > capacity)
    return 0;
  out[0] = 0x82;
  out[1] = 0x40;
  out[2] = 0x42;
  out[3] = 0x31;
  put_u16 (out + 4, (uint16_t) len);
  out[6] = dir;
  out[7 + len] = 0x82;
  out[8 + len] = 0x42;
  out[9 + len] = 0x42;
  return len + SIDETOUCH_SERIAL_DATA_OVERHEAD;
}

size_t
sidetouch_eagle_request (uint16_t sequence, const uint8_t *payload, size_t payload_length,
                         uint8_t *out, size_t capacity)
{
  size_t inner = 8 + payload_length, outer = 8 + inner, total;
  uint8_t *e;

  if (!out || (payload_length && !payload) || inner > UINT16_MAX - 6)
    return 0;
  total = serial_frame (0x00, outer, out, capacity);
  if (!total)
    return 0;
  put_u16 (out + 7, 0x0004);
  put_u16 (out + 9, (uint16_t) (inner + 6));
  out[11] = 0x2f;
  out[12] = 0x00;
  out[13] = 0x0b;
  out[14] = 0x00;
  e = out + 15;
  put_u16 (e, REQUEST_MAGIC);
  put_u16 (e + 2, sequence);
  put_u16 (e + 4, (uint16_t) inner);
  put_u16 (e + 6, 0);
  if (payload_length)
    memcpy (e + 8, payload, payload_length);
  put_u16 (e + 6, sidetouch_crc16 (e, inner));
  return total;
}

SidetouchProtoResult
sidetouch_eagle_response (const uint8_t *data, size_t length, uint16_t sequence,
                          const uint8_t **payload, size_t *payload_length)
{
  const uint8_t *e;
  size_t inner;
  uint16_t crc;

  /* 3 serial bytes + 4 wrapper bytes + 2 transport bytes ahead of an 8-byte frame */
  if (!data || length < 17)
    return SIDETOUCH_PROTO_TRUNCATED;
  if (length > SIDETOUCH_USB_PACKET)
    return SIDETOUCH_PROTO_LENGTH;
  if (data[2] != SERIAL_READ)
    return SIDETOUCH_PROTO_WRAPPER;
  if ((size_t) get_u16 (data + 3) + 3 != length)
    return SIDETOUCH_PROTO_LENGTH;
  if (data[5] != 0x1f || data[6] != 0x00 || (data[7] != 0x0b && data[7] != 0x8b) || data[8] != 0x00)
    return SIDETOUCH_PROTO_WRAPPER;
  e = data + 9;
  inner = get_u16 (e + 4);
  if (inner < 8 || inner != length - 9)
    return SIDETOUCH_PROTO_LENGTH;
  if (get_u16 (e) != RESPONSE_MAGIC)
    return SIDETOUCH_PROTO_MAGIC;
  if (get_u16 (e + 2) != sequence)
    return SIDETOUCH_PROTO_SEQUENCE;
  /* the CRC covers the whole frame with its own field taken as zero */
  crc = crc16_update (0xffff, e, 6);
  crc = crc16_update (crc, (const uint8_t[]) { 0, 0 }, 2);
  crc = crc16_update (crc, e + 8, inner - 8);
  if (crc != get_u16 (e + 6))
    return SIDETOUCH_PROTO_CRC;
  if (payload)
    *payload = e + 8;
  if (payload_length)
    *payload_length = inner - 8;
  return SIDETOUCH_PROTO_OK;
}

size_t
sidetouch_serial_poll (uint8_t *out, size_t capacity)
{
  if (!out || capacity < 1)
    return 0;
  out[0] = 0x81;
  return 1;
}

size_t
sidetouch_serial_length_request (uint8_t *out, size_t capacity)
{
  size_t n = out ? serial_frame (0x01, 2, out, capacity) : 0;

  if (n)
    out[7] = out[8] = 0xff;
  return n;
}

size_t
sidetouch_serial_data_request (uint16_t announced, uint8_t *out, size_t capacity)
{
  size_t n;

  if (!out || announced == 0 || announced > SIDETOUCH_SERIAL_MAX_ANNOUNCED)
    return 0;
  n = serial_frame (0x01, announced, out, capacity);
  if (n)
    memset (out + 7, 0xff, announced);
  return n;
}

SidetouchProtoResult
sidetouch_serial_length_reply (const uint8_t *data, size_t length, uint16_t *announced)
{
  uint16_t n;

  if (!data || length != 5)
    return SIDETOUCH_PROTO_TRUNCATED;
  if (data[2] != SERIAL_READ)
    return SIDETOUCH_PROTO_WRAPPER;
  n = get_u16 (data + 3);
  if (n == 0 || n > SIDETOUCH_SERIAL_MAX_ANNOUNCED)
    return SIDETOUCH_PROTO_LENGTH;
  if (announced)
    *announced = n;
  return SIDETOUCH_PROTO_OK;
}

SidetouchSerialState
sidetouch_serial_state (const uint8_t *data, size_t length, uint8_t *state)
{
  if (!data || length < 2)
    return SIDETOUCH_SERIAL_INVALID;
  if (length == 2)
    return SIDETOUCH_SERIAL_PENDING;
  if (state)
    *state = data[2];
  return SIDETOUCH_SERIAL_READY;
}

size_t
sidetouch_cmd_set_param (char parameter, uint32_t value, uint8_t *out, size_t capacity)
{
  if (!out || capacity < 6)
    return 0;
  out[0] = 0x02;
  out[1] = (uint8_t) parameter;
  put_u16 (out + 2, (uint16_t) value);
  put_u16 (out + 4, (uint16_t) (value >> 16));
  return 6;
}

size_t
sidetouch_cmd_range (uint16_t offset, uint16_t length, uint8_t *out, size_t capacity)
{
  if (!out || capacity < 5)
    return 0;
  out[0] = 0x0c;
  put_u16 (out + 1, offset);
  put_u16 (out + 3, length);
  return 5;
}

SidetouchProtoResult
sidetouch_capture_status (const uint8_t *payload, size_t length, uint16_t *word)
{
  if (!payload || length != 4)
    return SIDETOUCH_PROTO_LENGTH;
  if (payload[0] != 0 || payload[3] != 0)
    return SIDETOUCH_PROTO_STATUS;
  if (word)
    *word = get_u16 (payload + 1);
  return SIDETOUCH_PROTO_OK;
}

SidetouchProtoResult
sidetouch_capture_reply (const uint8_t *payload, size_t length, uint16_t *word, uint8_t *error_byte)
{
  if (!payload || length != 4)
    return SIDETOUCH_PROTO_LENGTH;
  if (payload[0] != 0)
    return SIDETOUCH_PROTO_STATUS;
  if (word)
    *word = get_u16 (payload + 1);
  if (error_byte)
    *error_byte = payload[3];
  return SIDETOUCH_PROTO_OK;
}

bool
sidetouch_untouched_status_accepted (uint16_t word, uint8_t error_byte)
{
  return error_byte == 0 && (word & 0x18) == 0x18;
}

bool
sidetouch_untouched_ready_ok (uint16_t word)
{
  return (word & 0x10) != 0;
}

bool
sidetouch_touch_status_ok (uint16_t word)
{
  return word == 0x001e || word == 0x005e;
}

bool
sidetouch_reference_status_ok (uint16_t word)
{
  return (word & 0x18) == 0x18 && (word & 0x04) == 0;
}

void
sidetouch_chunks_init (SidetouchChunks *c, uint8_t *data)
{
  c->data = data;
  c->length = 0;
  c->chunks = 0;
}

bool
sidetouch_chunks_next (const SidetouchChunks *c, size_t *offset, size_t *length)
{
  size_t left = SIDETOUCH_ACQUISITION_LENGTH - c->length;

  if (left == 0)
    return false;
  *offset = c->length;
  *length = left < SIDETOUCH_CHUNK_LENGTH ? left : SIDETOUCH_CHUNK_LENGTH;
  return true;
}

SidetouchProtoResult
sidetouch_chunks_append (SidetouchChunks *c, const uint8_t *payload, size_t length)
{
  size_t off, want;

  if (!payload || !sidetouch_chunks_next (c, &off, &want))
    return SIDETOUCH_PROTO_LENGTH;
  if (length != want + 1)
    return SIDETOUCH_PROTO_LENGTH;
  if (payload[0] != 0)
    return SIDETOUCH_PROTO_STATUS;
  memcpy (c->data + off, payload + 1, want);
  c->length += want;
  c->chunks++;
  return SIDETOUCH_PROTO_OK;
}

bool
sidetouch_chunks_complete (const SidetouchChunks *c)
{
  return c->length == SIDETOUCH_ACQUISITION_LENGTH && c->chunks == SIDETOUCH_ACQUISITION_CHUNKS;
}

SidetouchPollProgress
sidetouch_poll_progress (const uint8_t *data, size_t length, unsigned *attempts, uint8_t *state)
{
  if (!attempts)
    return SIDETOUCH_POLL_INVALID;
  switch (sidetouch_serial_state (data, length, state))
    {
    case SIDETOUCH_SERIAL_READY:
      return SIDETOUCH_POLL_READY;
    case SIDETOUCH_SERIAL_PENDING:
      (*attempts)++;
      return *attempts < SIDETOUCH_POLL_ATTEMPTS ? SIDETOUCH_POLL_RETRY : SIDETOUCH_POLL_EXHAUSTED;
    case SIDETOUCH_SERIAL_INVALID:
    default:
      return SIDETOUCH_POLL_INVALID;
    }
}

SidetouchWaitState
sidetouch_wait_state (uint8_t state)
{
  return (state & 0x20) ? SIDETOUCH_WAIT_PENDING : SIDETOUCH_WAIT_READY;
}

bool
sidetouch_cancel_state_ok (uint8_t state)
{
  return state == 0xd8 || state == 0xda;
}

bool
sidetouch_collector_init (SidetouchCollector *c, size_t expected)
{
  if (!c || expected < 2 || expected > sizeof c->bytes)
    return false;
  c->length = 0;
  c->expected = expected;
  c->reads = 0;
  return true;
}

SidetouchCollectResult
sidetouch_collector_feed (SidetouchCollector *c, const uint8_t *data, size_t length)
{
  if (!c || !data || length < 2 || length > SIDETOUCH_USB_PACKET || ++c->reads > SIDETOUCH_SERIAL_MAX_READS)
    return SIDETOUCH_COLLECT_ERROR;
  if (c->length == 0)
    {
      memcpy (c->bytes, data, 2);
      c->length = 2;
    }
  if (length - 2 > c->expected - c->length)
    return SIDETOUCH_COLLECT_ERROR;
  memcpy (c->bytes + c->length, data + 2, length - 2);
  c->length += length - 2;
  return c->length == c->expected ? SIDETOUCH_COLLECT_DONE : SIDETOUCH_COLLECT_MORE;
}

static bool
serial_body_is (const uint8_t *data, size_t length, const uint8_t *body, size_t body_length)
{
  return data && length == body_length + 2 && memcmp (data + 2, body, body_length) == 0;
}

bool
sidetouch_recorded_empty_response (const uint8_t *data, size_t length)
{
  static const uint8_t body[] = { SERIAL_READ, 0x04, 0x00, 0x1f, 0x00 };

  return serial_body_is (data, length, body, sizeof body);
}

bool
sidetouch_ready_drain_ok (const uint8_t *data, size_t length)
{
  static const uint8_t body[] = { SERIAL_READ, 0x04, 0x00, 0x07, 0x01 };

  return serial_body_is (data, length, body, sizeof body);
}

bool
sidetouch_zero_status (const uint8_t *payload, size_t length)
{
  return payload && length == 1 && payload[0] == 0;
}

SidetouchProtoResult
sidetouch_data_ready_status (const uint8_t *payload, size_t length, uint16_t *word)
{
  if (!payload || length != 3)
    return SIDETOUCH_PROTO_LENGTH;
  if (payload[0] != 0)
    return SIDETOUCH_PROTO_STATUS;
  if (word)
    *word = get_u16 (payload + 1);
  return SIDETOUCH_PROTO_OK;
}

bool
sidetouch_cancelled_status_ok (const uint8_t *payload, size_t length)
{
  return payload && length == 4 && payload[0] == 0 && payload[3] == 0x13;
}

bool
sidetouch_firmware_version (const uint8_t *payload, size_t length, uint8_t *major, uint8_t *minor)
{
  if (!payload || length < 3 || payload[0] != 0)
    return false;
  if (major)
    *major = payload[1];
  if (minor)
    *minor = payload[2];
  return true;
}

bool
sidetouch_runtime_info_ok (const uint8_t *payload, size_t length)
{
  static const struct { size_t at; uint16_t value; } layout[] = {
    { 1, 113 }, { 3, 115 }, { 5, 373 }, { 7, 8 }, { 11, 0x45b0 },
    { 13, SIDETOUCH_ACQUISITION_LENGTH }, { 15, 1 }, { 27, 4 },
  };

  if (!payload || length < SIDETOUCH_SETTINGS_BYTES || payload[0] != 0)
    return false;
  for (size_t i = 0; i < sizeof layout / sizeof *layout; i++)
    if (get_u16 (payload + layout[i].at) != layout[i].value)
      return false;
  return true;
}
