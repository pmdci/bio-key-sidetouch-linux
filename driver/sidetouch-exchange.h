/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * One SideTouch Eagle request/response exchange over the serial bridge, and
 * the bare serial read it is built from. Both are non-blocking, cancellable
 * FpiSsm transactions over an injected transport.
 *
 * Exchange sequence:
 *   two status polls (81, read state)       preflight
 *   the Eagle request frame, its write acknowledgement
 *   a status poll                           response ready
 *   length request, length reply            announced length
 *   data request, the response              assembled from packets
 * then the response is validated (envelope, wrapper, magic, sequence, CRC)
 * and its payload returned.
 *
 * A status read that returns transport bytes only is read again, up to
 * SIDETOUCH_POLL_ATTEMPTS times. Each logical serial read has one deadline,
 * the caller's timeout, across all the packets it takes.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-proto.h"
#include "sidetouch-transport.h"

#define SIDETOUCH_EXCHANGE_ERROR (sidetouch_exchange_error_quark ())

typedef enum {
  /* The reader returned the empty response it gives for a reused sequence
   * number: a valid envelope with no Eagle frame. */
  SIDETOUCH_EXCHANGE_ERROR_EMPTY_RESPONSE,
} SidetouchExchangeError;

GQuark sidetouch_exchange_error_quark (void);

/* Completion. On success data and length are the validated payload (or, for
 * sidetouch_serial_read_async, the whole serial read including its transport
 * pair), borrowed for the call. The callee owns error. */
typedef void (*SidetouchExchangeDone) (FpDevice     *device,
                                       const guint8 *data,
                                       gsize         length,
                                       GError       *error,
                                       gpointer      user_data);

void sidetouch_exchange_async (FpDevice                 *device,
                               const SidetouchTransport *transport,
                               guint16                   sequence,
                               const guint8             *payload,
                               gsize                     payload_length,
                               guint                     timeout_ms,
                               GCancellable             *cancellable,
                               SidetouchExchangeDone     done,
                               gpointer                  user_data);

/* Length request, length reply, data request and data, with the envelope
 * checked ("xx xx 62", exactly the announced length). */
void sidetouch_serial_read_async (FpDevice                 *device,
                                  const SidetouchTransport *transport,
                                  guint                     timeout_ms,
                                  GCancellable             *cancellable,
                                  SidetouchExchangeDone     done,
                                  gpointer                  user_data);

/* One logical serial read: packets assembled by a collector, with a single
 * deadline across all of them. Shared by the transactions built on the
 * exchange. */
typedef struct
{
  SidetouchCollector collector;
  gint64             deadline;
  gboolean           active;
} SidetouchSerialRead;

/* Starts the read on its first packet (later calls keep the started read).
 * FALSE if expected does not fit. */
gboolean sidetouch_serial_read_begin (SidetouchSerialRead *read,
                                      gsize                expected,
                                      guint                timeout_ms);

/* Milliseconds left before the read's deadline, at least 1; 0 once passed. */
guint sidetouch_serial_read_remaining (const SidetouchSerialRead *read);

/* Feeds one packet; on DONE or ERROR the read ends. */
SidetouchCollectResult sidetouch_serial_read_feed (SidetouchSerialRead *read,
                                                   const guint8        *data,
                                                   gsize                length);

/* A G_IO_ERROR_TIMED_OUT error naming what was read so far. */
GError *sidetouch_serial_read_timeout_error (const SidetouchSerialRead *read);
