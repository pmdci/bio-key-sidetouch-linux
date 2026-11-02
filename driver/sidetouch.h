/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The BIO-key SideTouch (2963:2001) libfprint device.
 *
 * Enrolment takes five touches, then builds the print's own background
 * reference from three untouched acquisitions (after a 1.5 s pause, checked by
 * guards G2 and G3, at most three attempts). The print stores the reader's
 * settings, that reference and the keypoint template. Verification and
 * identification check the settings, prepare the touch with each print's own
 * reference, and accept at a score of 7 or more.
 */
#pragma once

#include "drivers_api.h"
#include "sidetouch-open.h"
#include "sidetouch-transport.h"

#define FPI_TYPE_DEVICE_SIDETOUCH (fpi_device_sidetouch_get_type ())
G_DECLARE_DERIVABLE_TYPE (FpiDeviceSidetouch, fpi_device_sidetouch, FPI, DEVICE_SIDETOUCH, FpDevice)

struct _FpiDeviceSidetouchClass
{
  FpDeviceClass parent_class;
};

/* Replaces the USB transport and interface; for the offline tests, whose
 * subclass is a virtual device. */
void fpi_device_sidetouch_set_transport (FpiDeviceSidetouch          *self,
                                         const SidetouchTransport    *transport,
                                         const SidetouchInterfaceOps *interface,
                                         gpointer                     interface_backend);
