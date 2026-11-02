/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Wiping sensor-derived memory before it is released. The volatile writes
 * cannot be optimised away, unlike a memset before free.
 */
#pragma once

#include <stddef.h>

#include "kpcand.h"
#include "kptemplate.h"

static inline void
sidetouch_wipe (void *p, size_t n)
{
  volatile unsigned char *b = p;

  while (b && n--)
    *b++ = 0;
}

/* Wipes and frees a keypoint set. */
static inline void
sidetouch_wipe_set (KpSet *set)
{
  if (set->k)
    sidetouch_wipe (set->k, set->count * sizeof *set->k);
  kpc_free (set);
}

/* Wipes and frees a template's members. */
static inline void
sidetouch_wipe_template (KpTemplate *t)
{
  for (int i = 0; i < KPT_MAX_MEMBERS; i++)
    if (t->member[i].k)
      sidetouch_wipe (t->member[i].k, t->member[i].count * sizeof *t->member[i].k);
  kpt_free (t);
}
