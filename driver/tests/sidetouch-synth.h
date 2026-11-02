/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Synthetic format-4 streams for the SideTouch tests (no sensor data). */
#pragma once

#include <stdint.h>

#include "sidetouch-format4.h"

/* Encodes a grid whose horizontal steps outside the absolute rows and columns
 * fit a signed byte; returns 0, or -1 if a step does not fit. */
int synth_encode (const uint16_t grid[SIDETOUCH_PIXELS], uint8_t out[SIDETOUCH_FORMAT4_LENGTH]);

/* A deterministic encodable grid. */
void synth_grid (uint16_t grid[SIDETOUCH_PIXELS], unsigned seed);
