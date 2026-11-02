/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The driver's matching path: decoded samples and a per-print reference in,
 * a keypoint template or a decision out. It wraps the frozen keypoint
 * candidate (kpcand.h, kptemplate.h) without changing it: preparation
 * x = reference - current, SIFT extraction, SIGFM-style verification, and the
 * decision as the best member score, accepted above KPT_THRESHOLD (a score of
 * 7 or more). Pure computation, no I/O.
 */
#pragma once

#include <stdint.h>

#include "kptemplate.h"
#include "sidetouch-format4.h"
#include "sidetouch-reference.h"

typedef enum {
  SIDETOUCH_MATCH_OK = 0,
  /* The image gives no usable features (flat after subtraction, or fewer
   * than two distinct keypoint locations). The candidate cannot tell this
   * from an allocation failure inside it, so both are reported here. */
  SIDETOUCH_MATCH_UNSCORABLE = -1,
  SIDETOUCH_MATCH_TOO_FEW = -2,      /* enrolment: fewer than KPT_MIN_MEMBERS scorable touches */
  SIDETOUCH_MATCH_NO_MEMORY = -3,
  SIDETOUCH_MATCH_INVALID = -4,      /* a NULL argument */
} SidetouchMatchResult;

/* Features of one acquisition. On success the caller frees out with kpc_free;
 * on failure out holds nothing. */
SidetouchMatchResult sidetouch_match_features (const uint16_t samples[SIDETOUCH_PIXELS],
                                               const uint16_t reference[SIDETOUCH_PIXELS],
                                               KpSet         *out);

/* A template from the enrolment touches; unscorable touches are dropped, and
 * *dropped (if given) receives their count. On success the caller frees out
 * with kpt_free. */
SidetouchMatchResult sidetouch_match_enrol (const uint16_t *const touches[SIDETOUCH_ENROLL_TOUCHES],
                                            const uint16_t        reference[SIDETOUCH_PIXELS],
                                            KpTemplate           *out,
                                            int                  *dropped);

/* Verifies one acquisition against a template and its reference. An
 * unscorable probe returns SIDETOUCH_MATCH_UNSCORABLE with a rejecting
 * decision (score 0). */
SidetouchMatchResult sidetouch_match_verify (const KpTemplate *template,
                                             const uint16_t    reference[SIDETOUCH_PIXELS],
                                             const uint16_t    samples[SIDETOUCH_PIXELS],
                                             KptDecision      *decision);
