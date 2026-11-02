/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * One SideTouch acquisition, as a non-blocking FpiSsm transaction over an
 * injected transport.
 *
 * Touch mode (mode 2) waits for a finger; untouched mode (mode 3) acquires at
 * once, for the background reference. Both run:
 *
 *   01 41                runtime information (touch mode only; returned as
 *                        the settings a print stores)
 *   02 46, 02 45, 08 xx  setup: parameters F and E, then the mode
 *   81 ... fa .. da      wait polls, 50 ms apart, until ready
 *   serial read          the ready drain, exactly "62 04 00 07 01"
 *   01 43                capture status word (mode policy below)
 *   02 43 ff ff 00       clear it
 *   01 49                data-ready status word (mode policy below)
 *   0c x 41              range reads of the 13,696-byte stream
 * and decode it.
 *
 * Status policy: touch mode accepts exactly 0x001e or 0x005e for both words.
 * Untouched mode follows the reader's own rules: the flag register is cleared
 * (02 43) after every well-formed 01 43 reply, and the frame is refused unless
 * the error byte is 0 and bits 0x18 are set, and unless the 01 49 word has
 * bit 0x10. A refused frame ends the acquisition with
 * SIDETOUCH_ACQUIRE_ERROR_REFUSED and the reader state known. Bit 0x04 of the
 * 01 43 word (set on every accepted touch, so treated as contact) does not
 * stop an untouched acquisition: the frame is read out and the word returned,
 * for the reference guard to judge.
 *
 * Contact boundary. In touch mode it is the validated ready state: before it,
 * a cancellation stops the wait through the reader's own cancellation sequence
 * and the reader state stays known; from it on, a cancellation is deferred and
 * the acquisition finishes, and any failure leaves the reader state unknown
 * (a reconnect is needed). Untouched mode cannot be cancelled by the caller:
 * its boundary is the start of setup, and it has one deadline for the whole
 * acquisition. A wait still pending at the deadline (the reader holds mode 3
 * while a finger rests on the sensor) is stopped through the cancellation
 * sequence and reported as SIDETOUCH_ACQUIRE_ERROR_REFUSED with the reader
 * state known; if that sequence fails, the state is unknown. A deadline
 * anywhere else leaves the state unknown.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-format4.h"
#include "sidetouch-proto.h"
#include "sidetouch-transport.h"

#define SIDETOUCH_UNTOUCHED_DEADLINE_MS 3000

#define SIDETOUCH_ACQUIRE_ERROR (sidetouch_acquire_error_quark ())
GQuark sidetouch_acquire_error_quark (void);

typedef enum {
  SIDETOUCH_ACQUIRE_ERROR_REFUSED,   /* untouched mode: the reader refused the frame, or held its
                                      * wait until the deadline */
} SidetouchAcquireError;

typedef enum {
  SIDETOUCH_MODE_TOUCH,      /* 08 07, E = SIDETOUCH_TOUCH_TIMEOUT */
  SIDETOUCH_MODE_UNTOUCHED,  /* 08 05, E = SIDETOUCH_UNTOUCHED_TIMEOUT */
} SidetouchMode;

typedef enum {
  SIDETOUCH_READER_KNOWN,
  SIDETOUCH_READER_UNKNOWN,  /* failed past the contact boundary: reconnect */
} SidetouchReaderState;

typedef struct
{
  const uint16_t *samples;     /* SIDETOUCH_PIXELS decoded samples */
  uint16_t        status;      /* the 01 43 status word */
  const uint8_t  *settings;    /* touch mode: the 49-byte 01 41 payload; else NULL */
} SidetouchAcquisition;

typedef struct
{
  /* Touch mode: setup accepted, waiting for a finger. Optional. */
  void (*armed) (FpDevice *device, gpointer user_data);
  /* Touch mode: a finger arrived (the ready drain validated). Optional. */
  void (*contact) (FpDevice *device, gpointer user_data);
  /* Exactly once. On success result is borrowed for the call and error is
   * NULL; the callee owns error. A cancellation before the boundary reports
   * G_IO_ERROR_CANCELLED with the reader state known. */
  void (*done) (FpDevice                   *device,
                const SidetouchAcquisition *result,
                GError                     *error,
                SidetouchReaderState        reader,
                gpointer                    user_data);
} SidetouchAcquireCallbacks;

typedef struct _SidetouchAcquire SidetouchAcquire;

/* next_sequence belongs to the open session; every Eagle request advances it,
 * including the empty frame of a cancellation. The handle stays valid until
 * done has been called. */
SidetouchAcquire *sidetouch_acquire_start (FpDevice                        *device,
                                           const SidetouchTransport        *transport,
                                           guint16                         *next_sequence,
                                           SidetouchMode                    mode,
                                           guint                            timeout_ms,
                                           const SidetouchAcquireCallbacks *callbacks,
                                           gpointer                         user_data);

/* Idempotent; see the contact boundary above. Completion stays asynchronous. */
void sidetouch_acquire_cancel (SidetouchAcquire *acquire);
