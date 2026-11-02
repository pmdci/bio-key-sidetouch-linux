/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Opening the SideTouch reader: claim the interface, initialise the serial
 * bridge, then query the firmware (01 42), as one non-blocking transaction.
 *
 * The firmware query starts at the session's next sequence number. A reader
 * that still remembers that number from an earlier session answers with an
 * empty response; the number is then advanced and the query repeated, up to
 * the end of the 16-bit space. Any well-formed firmware reply is accepted;
 * a version other than 5.9, the only one tested, is logged as a warning.
 *
 * The interface is released before the outcome is reported on every failure
 * after a successful claim, and only then; on success it stays claimed for
 * the caller to release at close.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-transport.h"

#define SIDETOUCH_FIRMWARE_MAJOR 5
#define SIDETOUCH_FIRMWARE_MINOR 9

typedef struct
{
  /* FALSE with error set if the interface cannot be claimed. */
  gboolean (*claim) (gpointer backend, GError **error);
  /* Must tolerate failure. */
  void (*release) (gpointer backend);
  /* Optional, not used by sidetouch_open_async: the device's open calls it
   * after a failed open, to give up the handle the core opened for it. Must
   * tolerate failure. */
  void (*discard) (gpointer backend);
} SidetouchInterfaceOps;

/* Exactly once; the callee owns error. On success *next_sequence is the
 * number after the query's. */
typedef void (*SidetouchOpenDone) (FpDevice *device,
                                   GError   *error,
                                   gpointer  user_data);

void sidetouch_open_async (FpDevice                    *device,
                           const SidetouchTransport    *transport,
                           const SidetouchInterfaceOps *interface,
                           gpointer                     interface_backend,
                           guint16                     *next_sequence,
                           guint                        timeout_ms,
                           GCancellable                *cancellable,
                           SidetouchOpenDone            done,
                           gpointer                     user_data);
