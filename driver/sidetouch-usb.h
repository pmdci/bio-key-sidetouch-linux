/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The SideTouch transport over the device's own USB connection: bulk OUT
 * 0x02, bulk IN 0x81, vendor control OUT, an endpoint clear-halt on 0x81 for
 * IN recovery, and device timeouts for delays. The backend is the FpDevice.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-transport.h"

#define SIDETOUCH_EP_OUT 0x02
#define SIDETOUCH_EP_IN 0x81

extern const SidetouchTransportOps sidetouch_usb_ops;
