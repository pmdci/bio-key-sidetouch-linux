/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
#include "kptemplate.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char MAGIC[4] = {'S', 'K', 'P', 'T'};
enum { HEADER = 4 + 2 + 1 };

static int copy_set(const KpSet *in, KpSet *out)
{
  out->count = in->count;
  out->k = malloc(sizeof *out->k * (in->count ? in->count : 1));
  if (!out->k) return -1;
  memcpy(out->k, in->k, sizeof *out->k * in->count);
  return 0;
}

int kpt_enrol(const KpSet *const sets[], int count, KpTemplate *out)
{
  memset(out, 0, sizeof *out);
  for (int i = 0; i < count && out->members < KPT_MAX_MEMBERS; i++) {
    if (!sets[i] || sets[i]->count < 2 || sets[i]->count > KPT_MAX_KEYPOINTS) continue; /* dropped member */
    if (copy_set(sets[i], &out->member[out->members])) { kpt_free(out); return -1; }
    out->members++;
  }
  if (out->members < KPT_MIN_MEMBERS) { kpt_free(out); return -1; }
  return 0;
}

void kpt_free(KpTemplate *t)
{
  for (int i = 0; i < KPT_MAX_MEMBERS; i++) kpc_free(&t->member[i]);
  t->members = 0;
}

size_t kpt_serialised_size(const KpTemplate *t)
{
  size_t n = HEADER;
  for (int i = 0; i < t->members; i++) n += 2 + t->member[i].count * KPT_KEYPOINT_BYTES;
  return n;
}

static void put_u16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)(v >> 8); }
static unsigned get_u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static void put_f64(unsigned char *p, double v)
{
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  for (int i = 0; i < 8; i++) p[i] = (unsigned char)(bits >> (8 * i));
}

static double get_f64(const unsigned char *p)
{
  uint64_t bits = 0;
  for (int i = 0; i < 8; i++) bits |= (uint64_t)p[i] << (8 * i);
  double v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

size_t kpt_serialise(const KpTemplate *t, unsigned char *buf, size_t capacity)
{
  if (t->members < KPT_MIN_MEMBERS || t->members > KPT_MAX_MEMBERS) return 0;
  size_t n = kpt_serialised_size(t);
  if (n > capacity) return 0;
  memcpy(buf, MAGIC, 4);
  put_u16(buf + 4, KPT_VERSION);
  buf[6] = (unsigned char)t->members;
  unsigned char *p = buf + HEADER;
  for (int i = 0; i < t->members; i++) {
    const KpSet *s = &t->member[i];
    if (s->count < 2 || s->count > KPT_MAX_KEYPOINTS) return 0;
    put_u16(p, (unsigned)s->count);
    p += 2;
    for (size_t k = 0; k < s->count; k++) {
      put_f64(p, s->k[k].x);
      put_f64(p + 8, s->k[k].y);
      put_f64(p + 16, s->k[k].sigma);
      put_f64(p + 24, s->k[k].theta);
      memcpy(p + 32, s->k[k].d, KP_DESCRIPTOR);
      p += KPT_KEYPOINT_BYTES;
    }
  }
  return n;
}

int kpt_deserialise(const unsigned char *buf, size_t length, KpTemplate *out)
{
  memset(out, 0, sizeof *out);
  if (length < HEADER || memcmp(buf, MAGIC, 4) != 0 || get_u16(buf + 4) != KPT_VERSION) return -1;
  int members = buf[6];
  if (members < KPT_MIN_MEMBERS || members > KPT_MAX_MEMBERS) return -1;
  size_t at = HEADER;
  for (int i = 0; i < members; i++) {
    if (length - at < 2) { kpt_free(out); return -1; }
    size_t count = get_u16(buf + at);
    at += 2;
    if (count < 2 || count > KPT_MAX_KEYPOINTS || (length - at) / KPT_KEYPOINT_BYTES < count) { kpt_free(out); return -1; }
    KpSet *s = &out->member[i];
    s->k = malloc(sizeof *s->k * count);
    if (!s->k) { kpt_free(out); return -1; }
    s->count = count;
    out->members = i + 1;
    for (size_t k = 0; k < count; k++, at += KPT_KEYPOINT_BYTES) {
      Keypoint *kp = &s->k[k];
      kp->x = get_f64(buf + at);
      kp->y = get_f64(buf + at + 8);
      kp->sigma = get_f64(buf + at + 16);
      kp->theta = get_f64(buf + at + 24);
      memcpy(kp->d, buf + at + 32, KP_DESCRIPTOR);
      if (!isfinite(kp->x) || !isfinite(kp->y) || !isfinite(kp->sigma) || !isfinite(kp->theta)) { kpt_free(out); return -1; }
    }
  }
  if (at != length) { kpt_free(out); return -1; } /* trailing bytes */
  return 0;
}

KptDecision kpt_verify(const KpTemplate *t, const KpSet *probe)
{
  KptDecision d = {0, 0};
  for (int i = 0; i < t->members; i++) {
    KpcResult r = kpc_compare(probe, &t->member[i]);
    if (r.score > d.score) d.score = r.score;
  }
  d.accept = d.score > KPT_THRESHOLD;
  return d;
}
