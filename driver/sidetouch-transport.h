/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The injected transfer boundary of the SideTouch state machines.
 *
 * No state machine calls GUsb directly: each receives these operations, so the
 * same code runs over FpiUsbTransfer in the driver and over a scripted fake in
 * the offline tests. Every operation completes asynchronously, exactly once,
 * through the callback; none blocks the main loop.
 */
#pragma once

#include <gio/gio.h>

/* Completion of one operation. On success error is NULL, and for a bulk IN
 * data and length describe the received bytes, borrowed for the call only;
 * otherwise data is NULL and length 0. The callee owns error. A cancelled
 * operation reports G_IO_ERROR_CANCELLED. */
typedef void (*SidetouchTransferDone) (const guint8 *data,
                                       gsize         length,
                                       GError       *error,
                                       gpointer      user_data);

typedef struct
{
  /* Copies data before returning; the caller's buffer may be reused at once. */
  void (*bulk_send) (gpointer              backend,
                     const guint8         *data,
                     gsize                 length,
                     guint                 timeout_ms,
                     GCancellable         *cancellable,
                     SidetouchTransferDone done,
                     gpointer              user_data);
  /* Reads one packet of at most capacity bytes. */
  void (*bulk_receive) (gpointer              backend,
                        gsize                 capacity,
                        guint                 timeout_ms,
                        GCancellable         *cancellable,
                        SidetouchTransferDone done,
                        gpointer              user_data);
  /* Vendor control OUT with no data stage. */
  void (*control_out) (gpointer              backend,
                       guint8                request,
                       guint16               value,
                       guint16               index,
                       guint                 timeout_ms,
                       GCancellable         *cancellable,
                       SidetouchTransferDone done,
                       gpointer              user_data);
  /* Endpoint-level recovery of the IN pipe, used only by bridge
   * initialisation after a failed field sweep. */
  void (*recover_in) (gpointer              backend,
                      GCancellable         *cancellable,
                      SidetouchTransferDone done,
                      gpointer              user_data);
  /* A non-blocking delay. */
  void (*wait) (gpointer              backend,
                guint                 delay_ms,
                GCancellable         *cancellable,
                SidetouchTransferDone done,
                gpointer              user_data);
} SidetouchTransportOps;

typedef struct
{
  const SidetouchTransportOps *ops;
  gpointer                     backend;
} SidetouchTransport;
