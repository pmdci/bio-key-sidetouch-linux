/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/* Enrol, store, reload and verify for the keypoint matcher: background-
 * subtracted preparation, threshold t = 6.65225155379651, decision as
 * the maximum score over the template members.
 *
 * Serialised form (little-endian), validated as untrusted input on reload:
 *   "SKPT" | u16 version (1) | u8 members (3..5)
 *   per member: u16 keypoints (2..KPT_MAX_KEYPOINTS)
 *     per keypoint: f64 x | f64 y | f64 sigma | f64 theta | 128 descriptor bytes */
#ifndef KPTEMPLATE_H
#define KPTEMPLATE_H

#include "kpcand.h"

#include <stddef.h>

#define KPT_THRESHOLD 6.65225155379651 /* frozen development threshold */
enum {
  KPT_MAX_MEMBERS = 5,
  KPT_MIN_MEMBERS = 3,
  KPT_MAX_KEYPOINTS = 65535, /* the u16 count field; the frozen candidate itself has no cap */
  KPT_VERSION = 1,
  KPT_KEYPOINT_BYTES = 4 * 8 + KP_DESCRIPTOR
};

typedef struct {
  int members;
  KpSet member[KPT_MAX_MEMBERS];
} KpTemplate;

/* Copies the scorable sets (count >= 2 distinct locations is the caller's
 * kpc_extract result); NULL entries are skipped as dropped members, as is a
 * set beyond KPT_MAX_KEYPOINTS (not storable).
 * Returns 0, or -1 with fewer than three members. */
int kpt_enrol(const KpSet *const sets[], int count, KpTemplate *out);
void kpt_free(KpTemplate *t);

/* Returns the serialised length (0 if it does not fit or a member is out of bounds). */
size_t kpt_serialise(const KpTemplate *t, unsigned char *buf, size_t capacity);
size_t kpt_serialised_size(const KpTemplate *t);

/* Returns 0, or -1 for any malformed, truncated, oversized or wrong-version input. */
int kpt_deserialise(const unsigned char *buf, size_t length, KpTemplate *out);

typedef struct {
  int score;  /* best member score */
  int accept; /* score > KPT_THRESHOLD */
} KptDecision;

KptDecision kpt_verify(const KpTemplate *t, const KpSet *probe);

#endif
