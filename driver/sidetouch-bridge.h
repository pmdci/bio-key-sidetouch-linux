/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * SideTouch serial-bridge initialisation, as one non-blocking, cancellable
 * FpiSsm transaction over an injected transport. It runs once per open,
 * after the interface is claimed and before the first Eagle exchange.
 *
 *   control phase   vendor control OUTs with four bulk drains (contents unused)
 *   bulk preamble   five fixed frames around one control reset, then a status poll
 *   field sweep     poll until state bit 0x20 is set (at most 20 passes); each
 *                   pass reads a two-byte field and, for 3..512, a payload of
 *                   exactly that size, then waits 5 ms
 *   final phase     a seven-byte setup and its acknowledgement, a status poll,
 *                   and two fixed replies
 *
 * A failed sweep is retried twice. Before the second sweep the IN pipe is
 * recovered, then after 200 ms one readiness poll is tried (any outcome is
 * accepted) and 100 ms pass; before the third, 100 ms pass. The third sweep
 * is terminal: whatever its outcome, initialisation fails, as the
 * established sequence does.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-transport.h"

/* The callee owns error. */
typedef void (*SidetouchBridgeDone) (FpDevice *device,
                                     GError   *error,
                                     gpointer  user_data);

void sidetouch_bridge_init_async (FpDevice                 *device,
                                  const SidetouchTransport *transport,
                                  guint                     timeout_ms,
                                  GCancellable             *cancellable,
                                  SidetouchBridgeDone       done,
                                  gpointer                  user_data);
