/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Tests for sidetouch-proto: framing against recorded (non-biometric) frames. */
#include "check.h"
#include "sidetouch-proto.h"

/* A recorded firmware query and its response, sequence 1 (firmware 5.9). */
static const uint8_t fw_request[] = {
  0x82, 0x40, 0x42, 0x31, 0x12, 0x00, 0x00, 0x04, 0x00, 0x10, 0x00, 0x2f, 0x00, 0x0b, 0x00,
  0xba, 0xab, 0x01, 0x00, 0x0a, 0x00, 0xc4, 0xba, 0x01, 0x42, 0x82, 0x42, 0x42,
};
static const uint8_t fw_response[] = {
  0x32, 0x60, 0x62, 0x1b, 0x00, 0x1f, 0x00, 0x0b, 0x00,
  0xca, 0xf0, 0x01, 0x00, 0x15, 0x00, 0xc1, 0xd1,
  0x00, 0x05, 0x09, 0x68, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static void
test_crc (void)
{
  /* CRC-16/CCITT-FALSE check value */
  CHECK (sidetouch_crc16 ((const uint8_t *) "123456789", 9) == 0x29b1);
}

static void
test_eagle_request (void)
{
  uint8_t out[64];
  const uint8_t cmd[] = { 0x01, 0x42 };

  CHECK (sidetouch_eagle_request (1, cmd, sizeof cmd, out, sizeof out) == sizeof fw_request);
  CHECK_MEM (out, fw_request, sizeof fw_request);
  CHECK (sidetouch_eagle_request (1, cmd, sizeof cmd, out, sizeof fw_request - 1) == 0);
}

static void
test_eagle_response (void)
{
  uint8_t r[sizeof fw_response];
  const uint8_t *payload;
  size_t n;

  CHECK (sidetouch_eagle_response (fw_response, sizeof fw_response, 1, &payload, &n) == SIDETOUCH_PROTO_OK);
  CHECK (n == 13 && payload[0] == 0x00 && payload[1] == 0x05 && payload[2] == 0x09);

  /* the two leading transport bytes are not inspected */
  memcpy (r, fw_response, sizeof r);
  r[0] = 0x02;
  r[1] = 0x00;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_OK);
  /* the alternating wrapper byte */
  r[7] = 0x8b;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_OK);

  CHECK (sidetouch_eagle_response (fw_response, sizeof fw_response, 2, &payload, &n) == SIDETOUCH_PROTO_SEQUENCE);
  CHECK (sidetouch_eagle_response (fw_response, sizeof fw_response - 1, 1, &payload, &n) == SIDETOUCH_PROTO_LENGTH);
  CHECK (sidetouch_eagle_response (fw_response, 10, 1, &payload, &n) == SIDETOUCH_PROTO_TRUNCATED);

  memcpy (r, fw_response, sizeof r);
  r[2] = 0x63;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_WRAPPER);
  memcpy (r, fw_response, sizeof r);
  r[7] = 0x0c;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_WRAPPER);
  memcpy (r, fw_response, sizeof r);
  r[9] = 0xcb;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_MAGIC);
  memcpy (r, fw_response, sizeof r);
  r[sizeof r - 1] ^= 1;
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_CRC);
  memcpy (r, fw_response, sizeof r);
  r[13] = 0x16;   /* inner length disagrees with the serial length */
  CHECK (sidetouch_eagle_response (r, sizeof r, 1, &payload, &n) == SIDETOUCH_PROTO_LENGTH);
}

static void
test_serial_frames (void)
{
  uint8_t out[600];
  size_t n;
  const uint8_t length_req[] = { 0x82, 0x40, 0x42, 0x31, 0x02, 0x00, 0x01, 0xff, 0xff, 0x82, 0x42, 0x42 };
  const uint8_t data_req4[] = { 0x82, 0x40, 0x42, 0x31, 0x04, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0x82, 0x42, 0x42 };
  uint16_t announced;
  uint8_t state;

  CHECK (sidetouch_serial_poll (out, sizeof out) == 1 && out[0] == 0x81);
  n = sidetouch_serial_length_request (out, sizeof out);
  CHECK (n == sizeof length_req);
  CHECK_MEM (out, length_req, sizeof length_req);
  n = sidetouch_serial_data_request (4, out, sizeof out);
  CHECK (n == sizeof data_req4);
  CHECK_MEM (out, data_req4, sizeof data_req4);
  CHECK (sidetouch_serial_data_request (0, out, sizeof out) == 0);
  CHECK (sidetouch_serial_data_request (SIDETOUCH_SERIAL_MAX_ANNOUNCED + 1, out, sizeof out) == 0);

  CHECK (sidetouch_serial_length_reply ((const uint8_t[]) { 0x32, 0x60, 0x62, 0x04, 0x00 }, 5, &announced) == SIDETOUCH_PROTO_OK);
  CHECK (announced == 4);
  CHECK (sidetouch_serial_length_reply ((const uint8_t[]) { 0x32, 0x60, 0x63, 0x04, 0x00 }, 5, &announced) == SIDETOUCH_PROTO_WRAPPER);
  CHECK (sidetouch_serial_length_reply ((const uint8_t[]) { 0x32, 0x60, 0x62, 0x00, 0x00 }, 5, &announced) == SIDETOUCH_PROTO_LENGTH);
  CHECK (sidetouch_serial_length_reply ((const uint8_t[]) { 0x32, 0x60, 0x62, 0x04 }, 4, &announced) == SIDETOUCH_PROTO_TRUNCATED);

  CHECK (sidetouch_serial_state ((const uint8_t[]) { 0x32, 0x60, 0xfa }, 3, &state) == SIDETOUCH_SERIAL_READY && state == 0xfa);
  CHECK (sidetouch_serial_state ((const uint8_t[]) { 0x32, 0x60 }, 2, &state) == SIDETOUCH_SERIAL_PENDING);
  CHECK (sidetouch_serial_state ((const uint8_t[]) { 0x32 }, 1, &state) == SIDETOUCH_SERIAL_INVALID);
}

static void
test_commands (void)
{
  uint8_t out[8];

  CHECK (sidetouch_cmd_set_param ('E', 30000, out, sizeof out) == 6);
  CHECK_MEM (out, ((const uint8_t[]) { 0x02, 0x45, 0x30, 0x75, 0x00, 0x00 }), 6);
  CHECK (sidetouch_cmd_set_param ('F', 0x19, out, sizeof out) == 6);
  CHECK_MEM (out, ((const uint8_t[]) { 0x02, 0x46, 0x19, 0x00, 0x00, 0x00 }), 6);
  CHECK (sidetouch_cmd_range (0x0150, 0x0150, out, sizeof out) == 5);
  CHECK_MEM (out, ((const uint8_t[]) { 0x0c, 0x50, 0x01, 0x50, 0x01 }), 5);
  CHECK (sidetouch_cmd_range (0, 0x150, out, 4) == 0);
}

static void
test_capture_status (void)
{
  uint16_t word;

  CHECK (sidetouch_capture_status ((const uint8_t[]) { 0x00, 0x1a, 0x00, 0x00 }, 4, &word) == SIDETOUCH_PROTO_OK && word == 0x001a);
  CHECK (sidetouch_capture_status ((const uint8_t[]) { 0x00, 0x0a, 0x00, 0x13 }, 4, &word) == SIDETOUCH_PROTO_STATUS);
  CHECK (sidetouch_capture_status ((const uint8_t[]) { 0x01, 0x1e, 0x00, 0x00 }, 4, &word) == SIDETOUCH_PROTO_STATUS);
  CHECK (sidetouch_capture_status ((const uint8_t[]) { 0x00, 0x1e, 0x00 }, 3, &word) == SIDETOUCH_PROTO_LENGTH);

  /* mode 2: the exact words the Linux driver has accepted */
  CHECK (sidetouch_touch_status_ok (0x001e) && sidetouch_touch_status_ok (0x005e));
  CHECK (!sidetouch_touch_status_ok (0x001a) && !sidetouch_touch_status_ok (0x000a));
  /* mode 3 (reference frames): mask 0x18 required, bit 0x04 (contact correlate) clear */
  CHECK (sidetouch_reference_status_ok (0x001a) && sidetouch_reference_status_ok (0x005a));
  CHECK (!sidetouch_reference_status_ok (0x001e));   /* contact bit set */
  CHECK (!sidetouch_reference_status_ok (0x000a));   /* 0x10 missing */
  CHECK (!sidetouch_reference_status_ok (0x0012));   /* 0x08 missing */
}

static void
test_chunks (void)
{
  static uint8_t image[SIDETOUCH_ACQUISITION_LENGTH];
  uint8_t payload[1 + SIDETOUCH_CHUNK_LENGTH];
  SidetouchChunks c;
  size_t off, len;

  sidetouch_chunks_init (&c, image);
  for (int i = 0; i < SIDETOUCH_ACQUISITION_CHUNKS; i++)
    {
      CHECK (sidetouch_chunks_next (&c, &off, &len));
      payload[0] = 0;
      memset (payload + 1, i, len);
      if (i == 3)
        CHECK (sidetouch_chunks_append (&c, payload, len) == SIDETOUCH_PROTO_LENGTH);   /* missing status byte */
      CHECK (sidetouch_chunks_append (&c, payload, len + 1) == SIDETOUCH_PROTO_OK);
    }
  CHECK (len == 0x100);
  CHECK (!sidetouch_chunks_next (&c, &off, &len));
  CHECK (sidetouch_chunks_complete (&c));
  CHECK (image[0] == 0 && image[SIDETOUCH_ACQUISITION_LENGTH - 1] == 40);

  sidetouch_chunks_init (&c, image);
  CHECK (sidetouch_chunks_next (&c, &off, &len));
  payload[0] = 1;
  CHECK (sidetouch_chunks_append (&c, payload, len + 1) == SIDETOUCH_PROTO_STATUS);
  CHECK (!sidetouch_chunks_complete (&c));
}

/* Serial replies recorded with nothing on the sensor. */
static const uint8_t rec_pending[] = { 0x32, 0x60 };
static const uint8_t rec_wait[] = { 0x32, 0x60, 0xfa };
static const uint8_t rec_ready[] = { 0x32, 0x60, 0xda };
static const uint8_t rec_drain[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x07, 0x01 };

static void
test_poll (void)
{
  unsigned attempts = 0;
  uint8_t state = 0;

  CHECK (sidetouch_poll_progress (rec_wait, sizeof rec_wait, &attempts, &state) == SIDETOUCH_POLL_READY);
  CHECK (state == 0xfa && attempts == 0);
  /* transport-only replies: read again, at most four reads in all */
  for (int i = 1; i < SIDETOUCH_POLL_ATTEMPTS; i++)
    CHECK (sidetouch_poll_progress (rec_pending, sizeof rec_pending, &attempts, NULL) == SIDETOUCH_POLL_RETRY);
  CHECK (sidetouch_poll_progress (rec_pending, sizeof rec_pending, &attempts, NULL) == SIDETOUCH_POLL_EXHAUSTED);
  attempts = 0;
  CHECK (sidetouch_poll_progress (rec_pending, 1, &attempts, NULL) == SIDETOUCH_POLL_INVALID);
  CHECK (sidetouch_poll_progress (NULL, 3, &attempts, NULL) == SIDETOUCH_POLL_INVALID);
  CHECK (sidetouch_poll_progress (rec_ready, sizeof rec_ready, NULL, NULL) == SIDETOUCH_POLL_INVALID);

  CHECK (sidetouch_wait_state (0xfa) == SIDETOUCH_WAIT_PENDING);
  CHECK (sidetouch_wait_state (0xda) == SIDETOUCH_WAIT_READY);
  /* only bit 0x20 is read: set pending, clear ready (the drain confirms) */
  CHECK (sidetouch_wait_state (0x04) == SIDETOUCH_WAIT_READY);
  CHECK (sidetouch_wait_state (0xd8) == SIDETOUCH_WAIT_READY);
  CHECK (sidetouch_wait_state (0x00) == SIDETOUCH_WAIT_READY);
  CHECK (sidetouch_wait_state (0x20) == SIDETOUCH_WAIT_PENDING);
  CHECK (sidetouch_wait_state (0x2a) == SIDETOUCH_WAIT_PENDING);
  CHECK (sidetouch_cancel_state_ok (0xd8) && sidetouch_cancel_state_ok (0xda) && !sidetouch_cancel_state_ok (0xfa));
}

static void
test_collector (void)
{
  SidetouchCollector c;
  /* the firmware response split after the wrapper: the second packet carries
   * its own transport pair, which is dropped */
  uint8_t second[2 + sizeof fw_response - 9];

  CHECK (sidetouch_collector_init (&c, sizeof fw_response));
  CHECK (sidetouch_collector_feed (&c, rec_pending, sizeof rec_pending) == SIDETOUCH_COLLECT_MORE);
  CHECK (sidetouch_collector_feed (&c, fw_response, 9) == SIDETOUCH_COLLECT_MORE);
  second[0] = 0x32;
  second[1] = 0x60;
  memcpy (second + 2, fw_response + 9, sizeof fw_response - 9);
  CHECK (sidetouch_collector_feed (&c, second, sizeof second) == SIDETOUCH_COLLECT_DONE);
  CHECK (c.length == sizeof fw_response);
  CHECK_MEM (c.bytes, fw_response, sizeof fw_response);

  /* one packet more than expected is refused, not truncated */
  CHECK (sidetouch_collector_init (&c, 5));
  CHECK (sidetouch_collector_feed (&c, fw_response, 6) == SIDETOUCH_COLLECT_ERROR);
  /* a packet shorter than the transport pair */
  CHECK (sidetouch_collector_init (&c, 5));
  CHECK (sidetouch_collector_feed (&c, fw_response, 1) == SIDETOUCH_COLLECT_ERROR);
  /* the read bound: 1000 reads allowed, the 1001st refused */
  CHECK (sidetouch_collector_init (&c, 5));
  for (unsigned i = 0; i < SIDETOUCH_SERIAL_MAX_READS; i++)
    if (sidetouch_collector_feed (&c, rec_pending, sizeof rec_pending) != SIDETOUCH_COLLECT_MORE)
      {
        CHECK (0 /* read bound too small */);
        break;
      }
  CHECK (sidetouch_collector_feed (&c, rec_pending, sizeof rec_pending) == SIDETOUCH_COLLECT_ERROR);
  CHECK (!sidetouch_collector_init (&c, 1));
  CHECK (!sidetouch_collector_init (&c, sizeof c.bytes + 1));
}

static void
test_special_replies (void)
{
  const uint8_t empty[] = { 0x32, 0x60, 0x62, 0x04, 0x00, 0x1f, 0x00 };
  uint8_t t[8];

  CHECK (sidetouch_ready_drain_ok (rec_drain, sizeof rec_drain));
  memcpy (t, rec_drain, sizeof rec_drain);
  t[0] = 0x33;                    /* transport bytes are not interpreted */
  CHECK (sidetouch_ready_drain_ok (t, sizeof rec_drain));
  t[6] = 0x00;
  CHECK (!sidetouch_ready_drain_ok (t, sizeof rec_drain));
  CHECK (!sidetouch_ready_drain_ok (rec_drain, sizeof rec_drain - 1));

  CHECK (sidetouch_recorded_empty_response (empty, sizeof empty));
  CHECK (!sidetouch_recorded_empty_response (rec_drain, sizeof rec_drain));
  CHECK (!sidetouch_recorded_empty_response (fw_response, sizeof fw_response));
}

static void
test_payloads (void)
{
  uint8_t info[49];
  uint8_t major = 0, minor = 0;
  uint16_t word = 0;
  const uint8_t cancelled[] = { 0x00, 0x0a, 0x00, 0x13 };
  const uint8_t cancelled_4a[] = { 0x00, 0x4a, 0x00, 0x13 };
  const uint8_t ready_1a[] = { 0x00, 0x1a, 0x00 };

  CHECK (sidetouch_zero_status ((const uint8_t[]) { 0 }, 1));
  CHECK (!sidetouch_zero_status ((const uint8_t[]) { 1 }, 1));
  CHECK (!sidetouch_zero_status ((const uint8_t[]) { 0, 0 }, 2));

  CHECK (sidetouch_data_ready_status (ready_1a, sizeof ready_1a, &word) == SIDETOUCH_PROTO_OK && word == 0x001a);
  CHECK (sidetouch_data_ready_status ((const uint8_t[]) { 1, 0x1e, 0 }, 3, &word) == SIDETOUCH_PROTO_STATUS);
  CHECK (sidetouch_data_ready_status (ready_1a, 2, &word) == SIDETOUCH_PROTO_LENGTH);

  CHECK (sidetouch_cancelled_status_ok (cancelled, sizeof cancelled));
  CHECK (sidetouch_cancelled_status_ok (cancelled_4a, sizeof cancelled_4a));
  /* the word is not checked: 0x0002 is the untouched-mode form */
  CHECK (sidetouch_cancelled_status_ok ((const uint8_t[]) { 0x00, 0x02, 0x00, 0x13 }, 4));
  CHECK (sidetouch_cancelled_status_ok ((const uint8_t[]) { 0x00, 0x1e, 0x01, 0x13 }, 4));
  CHECK (!sidetouch_cancelled_status_ok ((const uint8_t[]) { 0x00, 0x0a, 0x00, 0x00 }, 4));
  CHECK (!sidetouch_cancelled_status_ok ((const uint8_t[]) { 0x00, 0x0a, 0x00, 0x07 }, 4));
  CHECK (!sidetouch_cancelled_status_ok ((const uint8_t[]) { 0x01, 0x0a, 0x00, 0x13 }, 4));
  CHECK (!sidetouch_cancelled_status_ok (cancelled, 3));
  CHECK (!sidetouch_cancelled_status_ok (NULL, 4));

  /* fw_response payload: 00 05 09 ... -> firmware 5.9 */
  CHECK (sidetouch_firmware_version (fw_response + 17, sizeof fw_response - 17, &major, &minor));
  CHECK (major == 5 && minor == 9);
  CHECK (!sidetouch_firmware_version ((const uint8_t[]) { 1, 5, 9 }, 3, &major, &minor));
  CHECK (!sidetouch_firmware_version (fw_response + 17, 2, &major, &minor));

  /* runtime information: the established layout fields */
  memset (info, 0, sizeof info);
  {
    const struct { int at; unsigned v; } f[] = {
      { 1, 113 }, { 3, 115 }, { 5, 373 }, { 7, 8 }, { 11, 0x45b0 }, { 13, 0x3580 }, { 15, 1 }, { 27, 4 },
    };

    for (size_t i = 0; i < sizeof f / sizeof *f; i++)
      {
        info[f[i].at] = (uint8_t) f[i].v;
        info[f[i].at + 1] = (uint8_t) (f[i].v >> 8);
      }
    CHECK (sidetouch_runtime_info_ok (info, sizeof info));
    CHECK (!sidetouch_runtime_info_ok (info, sizeof info - 1));
    for (size_t i = 0; i < sizeof f / sizeof *f; i++)
      {
        info[f[i].at] ^= 1;
        if (sidetouch_runtime_info_ok (info, sizeof info))
          CHECK (0 /* layout field not checked */);
        info[f[i].at] ^= 1;
      }
    info[0] = 1;
    CHECK (!sidetouch_runtime_info_ok (info, sizeof info));
  }
}

int
main (void)
{
  test_crc ();
  test_eagle_request ();
  test_eagle_response ();
  test_serial_frames ();
  test_commands ();
  test_capture_status ();
  test_chunks ();
  test_poll ();
  test_collector ();
  test_special_replies ();
  test_payloads ();
  CHECK_DONE ("test-sidetouch-proto");
}
