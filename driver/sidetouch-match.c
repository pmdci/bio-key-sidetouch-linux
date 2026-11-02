/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The driver's matching path. See sidetouch-match.h.
 */
#include "sidetouch-match.h"
#include "sidetouch-wipe.h"

#include <stdlib.h>
#include <string.h>

/* kpc_prepare works on 32-bit samples and writes doubles; about 200 KB, so
 * heap-allocated per call rather than static, which keeps the path
 * reentrant across devices. */
typedef struct {
  int32_t current[SIDETOUCH_PIXELS];
  int32_t reference[SIDETOUCH_PIXELS];
  double  prepared[SIDETOUCH_PIXELS];
} Workspace;

static SidetouchMatchResult
features (Workspace *w, const uint16_t *samples, KpSet *out)
{
  for (int k = 0; k < SIDETOUCH_PIXELS; k++)
    w->current[k] = samples[k];
  if (kpc_prepare (w->current, w->reference, w->prepared) != 0)
    return SIDETOUCH_MATCH_UNSCORABLE;
  if (kpc_extract (w->prepared, out) != 0)
    {
      sidetouch_wipe_set (out);
      return SIDETOUCH_MATCH_UNSCORABLE;
    }
  return SIDETOUCH_MATCH_OK;
}

static Workspace *
workspace_new (const uint16_t *reference)
{
  Workspace *w = malloc (sizeof *w);

  if (w)
    for (int k = 0; k < SIDETOUCH_PIXELS; k++)
      w->reference[k] = reference[k];
  return w;
}

/* The workspace holds sensor samples; wipe it before release. */
static void
workspace_free (Workspace *w)
{
  if (!w)
    return;
  sidetouch_wipe (w, sizeof *w);
  free (w);
}

SidetouchMatchResult
sidetouch_match_features (const uint16_t samples[SIDETOUCH_PIXELS],
                          const uint16_t reference[SIDETOUCH_PIXELS],
                          KpSet         *out)
{
  Workspace *w;
  SidetouchMatchResult r;

  if (!out)
    return SIDETOUCH_MATCH_INVALID;
  out->k = NULL;
  out->count = 0;
  if (!samples || !reference)
    return SIDETOUCH_MATCH_INVALID;
  w = workspace_new (reference);
  if (!w)
    return SIDETOUCH_MATCH_NO_MEMORY;
  r = features (w, samples, out);
  workspace_free (w);
  return r;
}

SidetouchMatchResult
sidetouch_match_enrol (const uint16_t *const touches[SIDETOUCH_ENROLL_TOUCHES],
                       const uint16_t        reference[SIDETOUCH_PIXELS],
                       KpTemplate           *out,
                       int                  *dropped)
{
  KpSet sets[SIDETOUCH_ENROLL_TOUCHES];
  const KpSet *in[SIDETOUCH_ENROLL_TOUCHES];
  SidetouchMatchResult r = SIDETOUCH_MATCH_OK;
  Workspace *w;
  int lost = 0;

  if (!out)
    return SIDETOUCH_MATCH_INVALID;
  memset (out, 0, sizeof *out);
  if (!touches || !reference)
    return SIDETOUCH_MATCH_INVALID;
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    if (!touches[i])
      return SIDETOUCH_MATCH_INVALID;
  memset (sets, 0, sizeof sets);
  w = workspace_new (reference);
  if (!w)
    return SIDETOUCH_MATCH_NO_MEMORY;

  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    {
      int ok = features (w, touches[i], &sets[i]) == SIDETOUCH_MATCH_OK;

      in[i] = ok ? &sets[i] : NULL;
      lost += !ok;
    }
  workspace_free (w);

  /* kpt_enrol copies the sets; it fails only with fewer than three members
   * or on allocation failure, which the member count tells apart */
  if (SIDETOUCH_ENROLL_TOUCHES - lost < KPT_MIN_MEMBERS)
    r = SIDETOUCH_MATCH_TOO_FEW;
  else if (kpt_enrol (in, SIDETOUCH_ENROLL_TOUCHES, out) != 0)
    r = SIDETOUCH_MATCH_NO_MEMORY;
  for (int i = 0; i < SIDETOUCH_ENROLL_TOUCHES; i++)
    sidetouch_wipe_set (&sets[i]);
  if (dropped)
    *dropped = lost;
  return r;
}

SidetouchMatchResult
sidetouch_match_verify (const KpTemplate *template,
                        const uint16_t    reference[SIDETOUCH_PIXELS],
                        const uint16_t    samples[SIDETOUCH_PIXELS],
                        KptDecision      *decision)
{
  KpSet probe;
  SidetouchMatchResult r;

  if (!decision)
    return SIDETOUCH_MATCH_INVALID;
  decision->score = 0;
  decision->accept = 0;
  if (!template || template->members < KPT_MIN_MEMBERS || template->members > KPT_MAX_MEMBERS)
    return SIDETOUCH_MATCH_INVALID;
  r = sidetouch_match_features (samples, reference, &probe);
  if (r != SIDETOUCH_MATCH_OK)
    return r;
  *decision = kpt_verify (template, &probe);
  sidetouch_wipe_set (&probe);
  return SIDETOUCH_MATCH_OK;
}
