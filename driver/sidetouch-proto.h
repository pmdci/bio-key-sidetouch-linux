/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * BIO-key SideTouch (2963:2001) wire protocol: pure framing, no I/O.
 *
 * Layers, outermost first:
 *  - serial bridge: writes are "82 40 42 31 <len u16> <dir> <bytes> 82 42 42";
 *    a one-byte poll (0x81) returns "xx xx <state>"; reads announce their
 *    length ("xx xx 62 <len u16>") and then return "xx xx 62 <len u16> <bytes>".
 *    The two leading transport bytes are never interpreted.
 *  - outer wrapper: "04 00 <inner + 6 u16> 2f 00 0b 00" before each request,
 *    "1f 00 <0b|8b> 00" before each response.
 *  - Eagle frame: magic (0xabba request, 0xf0ca response), sequence, total
 *    length and CRC-16/CCITT-FALSE (computed with the CRC field zero), then the
 *    command or response payload.
 *
 * Every function validates lengths before indexing; device input is untrusted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIDETOUCH_USB_PACKET 512
#define SIDETOUCH_ACQUISITION_LENGTH 0x3580   /* 13,696 bytes, one format-4 stream */
#define SIDETOUCH_CHUNK_LENGTH 0x150
#define SIDETOUCH_ACQUISITION_CHUNKS 41
#define SIDETOUCH_SERIAL_DATA_OVERHEAD 10
#define SIDETOUCH_SERIAL_MAX_ANNOUNCED (SIDETOUCH_USB_PACKET - SIDETOUCH_SERIAL_DATA_OVERHEAD)

typedef enum {
  SIDETOUCH_PROTO_OK = 0,
  SIDETOUCH_PROTO_TRUNCATED = -1,
  SIDETOUCH_PROTO_LENGTH = -2,
  SIDETOUCH_PROTO_WRAPPER = -3,
  SIDETOUCH_PROTO_MAGIC = -4,
  SIDETOUCH_PROTO_SEQUENCE = -5,
  SIDETOUCH_PROTO_CRC = -6,
  SIDETOUCH_PROTO_STATUS = -7,
} SidetouchProtoResult;

typedef enum {
  SIDETOUCH_SERIAL_INVALID = -1,
  SIDETOUCH_SERIAL_PENDING = 0,   /* transport bytes only: read again */
  SIDETOUCH_SERIAL_READY = 1,     /* a state byte is present */
} SidetouchSerialState;

uint16_t sidetouch_crc16 (const uint8_t *data,
                          size_t         length);

/* A complete Bulk-OUT transfer carrying one Eagle request. Returns its length,
 * or 0 if it does not fit or the payload is too long. */
size_t sidetouch_eagle_request (uint16_t       sequence,
                                const uint8_t *payload,
                                size_t         payload_length,
                                uint8_t       *out,
                                size_t         capacity);

/* Validates one complete serial read carrying an Eagle response and returns
 * the payload (borrowed from data). */
SidetouchProtoResult sidetouch_eagle_response (const uint8_t  *data,
                                               size_t          length,
                                               uint16_t        sequence,
                                               const uint8_t **payload,
                                               size_t         *payload_length);

/* Serial-bridge frames. Each returns the frame length, or 0 if it does not fit. */
size_t sidetouch_serial_poll (uint8_t *out,
                              size_t   capacity);
size_t sidetouch_serial_length_request (uint8_t *out,
                                        size_t   capacity);
size_t sidetouch_serial_data_request (uint16_t announced,
                                      uint8_t *out,
                                      size_t   capacity);

SidetouchProtoResult sidetouch_serial_length_reply (const uint8_t *data,
                                                    size_t         length,
                                                    uint16_t      *announced);
SidetouchSerialState sidetouch_serial_state (const uint8_t *data,
                                             size_t         length,
                                             uint8_t       *state);

/* Command payloads. "02 <p> <u32 value>" sets common parameter p; the
 * established acquisition uses 'F' = 0x19 and 'E' = the mode's timeout value. */
size_t sidetouch_cmd_set_param (char     parameter,
                                uint32_t value,
                                uint8_t *out,
                                size_t   capacity);
/* "0c <offset u16> <length u16>": one range read of the acquisition buffer. */
size_t sidetouch_cmd_range (uint16_t offset,
                            uint16_t length,
                            uint8_t *out,
                            size_t   capacity);

extern const uint8_t sidetouch_cmd_info_b[2];      /* 01 42: firmware version */
extern const uint8_t sidetouch_cmd_info_a[2];      /* 01 41: runtime sensor information */
extern const uint8_t sidetouch_cmd_status[2];      /* 01 43: capture status word */
extern const uint8_t sidetouch_cmd_clear[5];       /* 02 43 ff ff 00 */
extern const uint8_t sidetouch_cmd_ready[2];       /* 01 49: read before range reads */
extern const uint8_t sidetouch_cmd_touch[2];       /* 08 07: touch-triggered acquisition (mode 2) */
extern const uint8_t sidetouch_cmd_untouched[2];   /* 08 05: acquisition without touch wait (mode 3) */

#define SIDETOUCH_TOUCH_TIMEOUT 10000000u          /* 'E' for mode 2 */
#define SIDETOUCH_UNTOUCHED_TIMEOUT 30000u         /* 'E' for mode 3 */

/* Parses the 01 43 payload (response status 0, status word, error byte 0). */
SidetouchProtoResult sidetouch_capture_status (const uint8_t *payload,
                                               size_t         length,
                                               uint16_t      *word);

/* A touch acquisition: exactly the words observed on every accepted touch. */
bool sidetouch_touch_status_ok (uint16_t word);

/* Parses the 01 43 payload in untouched mode: response status 0, then the
 * status word and the device error byte, both returned for the caller's
 * policy. */
SidetouchProtoResult sidetouch_capture_reply (const uint8_t *payload,
                                              size_t         length,
                                              uint16_t      *word,
                                              uint8_t       *error_byte);

/* An untouched frame the reader itself accepts: error byte 0 and bits 0x18
 * set in the 01 43 word; bit 0x10 set in the 01 49 word. Otherwise the reader
 * has refused the frame. */
bool sidetouch_untouched_status_accepted (uint16_t word,
                                          uint8_t  error_byte);
bool sidetouch_untouched_ready_ok (uint16_t word);

/* The reference guard G1 on one frame's 01 43 word: bits 0x18 set, the
 * observed contact correlate 0x04 clear. The 0x04 test is a candidate
 * heuristic, not a decoded meaning. */
bool sidetouch_reference_status_ok (uint16_t word);

/* Assembles the 41 range reads of one acquisition, in order. */
typedef struct {
  uint8_t *data;   /* SIDETOUCH_ACQUISITION_LENGTH bytes */
  size_t   length;
  int      chunks;
} SidetouchChunks;

void sidetouch_chunks_init (SidetouchChunks *c,
                            uint8_t         *data);
/* The next range to request; false when complete. */
bool sidetouch_chunks_next (const SidetouchChunks *c,
                            size_t                *offset,
                            size_t                *length);
/* The range response payload: a zero status byte, then exactly the bytes asked for. */
SidetouchProtoResult sidetouch_chunks_append (SidetouchChunks *c,
                                              const uint8_t   *payload,
                                              size_t           length);
bool sidetouch_chunks_complete (const SidetouchChunks *c);

/* ---- Serial-bridge read policy -------------------------------------------
 *
 * Every IN packet starts with two transport bytes that carry no serial data
 * and are never interpreted. A two-byte packet therefore means "nothing yet".
 */

#define SIDETOUCH_POLL_ATTEMPTS 4       /* reads of one poll reply before giving up */
#define SIDETOUCH_PREFLIGHT_POLLS 2     /* status polls before every Eagle request */
#define SIDETOUCH_SERIAL_MAX_READS 1000 /* packets per logical serial read */
#define SIDETOUCH_SERIAL_LENGTH_REPLY 5 /* "xx xx 62 <len u16>" */

typedef enum {
  SIDETOUCH_POLL_READY,      /* a state byte is present */
  SIDETOUCH_POLL_RETRY,      /* transport bytes only: read again without rewriting the poll */
  SIDETOUCH_POLL_EXHAUSTED,  /* SIDETOUCH_POLL_ATTEMPTS reads without a state byte */
  SIDETOUCH_POLL_INVALID,
} SidetouchPollProgress;

/* One read of a status poll or write acknowledgement. *attempts is zeroed by
 * the caller before the poll is written. READY stores the state byte. */
SidetouchPollProgress sidetouch_poll_progress (const uint8_t *data,
                                               size_t         length,
                                               unsigned      *attempts,
                                               uint8_t       *state);

/* Serial state after a capture setup: only bit 0x20 is read. Set (0xfa) the
 * reader is still working; clear (0xda, and any other value with the bit
 * clear) it has a reply to send, and the ready drain that follows is what
 * confirms it. In mode 3 ready also arrives with nothing on the sensor, so it
 * is not a contact signal there. */
typedef enum {
  SIDETOUCH_WAIT_PENDING = 0,
  SIDETOUCH_WAIT_READY = 1,
} SidetouchWaitState;

SidetouchWaitState sidetouch_wait_state (uint8_t state);

/* Serial state polled during cancellation: 0xd8 or 0xda. */
bool sidetouch_cancel_state_ok (uint8_t state);

/* Assembles one logical serial read of exactly `expected` bytes from IN
 * packets: the first packet's transport pair is kept (so the result has the
 * "xx xx 62 ..." form the parsers expect), later packets' pairs are dropped. */
typedef struct {
  uint8_t  bytes[SIDETOUCH_USB_PACKET + 3];
  size_t   length;
  size_t   expected;
  unsigned reads;
} SidetouchCollector;

typedef enum {
  SIDETOUCH_COLLECT_MORE,    /* read another packet */
  SIDETOUCH_COLLECT_DONE,    /* bytes[0..expected) is complete */
  SIDETOUCH_COLLECT_ERROR,   /* oversized, malformed or over the read bound */
} SidetouchCollectResult;

bool sidetouch_collector_init (SidetouchCollector *c,
                               size_t              expected);
SidetouchCollectResult sidetouch_collector_feed (SidetouchCollector *c,
                                                 const uint8_t      *data,
                                                 size_t              length);

/* The seven-byte reply recorded when an Eagle sequence number is reused: a
 * valid serial envelope ("62 04 00 1f 00") with no Eagle frame. */
bool sidetouch_recorded_empty_response (const uint8_t *data,
                                        size_t         length);

/* The capture-ready drain after a ready wait state: exactly "62 04 00 07 01"
 * after the transport pair. */
bool sidetouch_ready_drain_ok (const uint8_t *data,
                               size_t         length);

/* ---- Eagle response payloads --------------------------------------------- */

/* A one-byte zero status: the reply to every parameter write and setup. */
bool sidetouch_zero_status (const uint8_t *payload,
                            size_t         length);

/* "01 49": zero status then the status word (mode 2 0x001e/0x005e, mode 3
 * 0x001a observed). The word is returned for the caller's mode policy. */
SidetouchProtoResult sidetouch_data_ready_status (const uint8_t *payload,
                                                  size_t         length,
                                                  uint16_t      *word);

/* "01 43" after a cancellation, in any mode: zero status and error byte 0x13.
 * The status word between them is not checked; it varies with the mode
 * (0x000a or 0x004a after a touch wait, 0x0002 after an untouched wait). */
bool sidetouch_cancelled_status_ok (const uint8_t *payload,
                                    size_t         length);

/* "01 42": zero status, then firmware major and minor. */
bool sidetouch_firmware_version (const uint8_t *payload,
                                 size_t         length,
                                 uint8_t       *major,
                                 uint8_t       *minor);

/* "01 41": zero status and the established layout (113 x 115 samples, 373 dpi,
 * 8-byte field, 0x45b0, a 13,696-byte acquisition, format selector 1, 4 at
 * 27). At least SIDETOUCH_SETTINGS_BYTES long. */
#define SIDETOUCH_SETTINGS_BYTES 49
bool sidetouch_runtime_info_ok (const uint8_t *payload,
                                size_t         length);
