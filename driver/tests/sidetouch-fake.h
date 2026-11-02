/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Scripted transport for the SideTouch state-machine tests.
 *
 * A script is a list of steps, consumed in order. Each operation the machine
 * starts must match the next step's kind (and, for a send or control step
 * with data, its exact bytes or fields); the step then completes from an idle
 * source with its scripted reply or error, so a machine that blocked the main
 * loop could never finish. A mismatch or an operation past the end of the
 * script is recorded and reported as an error to the machine.
 */
#pragma once

#include "sidetouch-transport.h"

typedef enum {
  FAKE_SEND,
  FAKE_RECEIVE,
  FAKE_CONTROL,
  FAKE_RECOVER,
  FAKE_WAIT,
} FakeKind;

typedef struct
{
  FakeKind      kind;
  const guint8 *data;      /* SEND: expected bytes (NULL: any); RECEIVE: the reply */
  gsize         length;
  gint          error;     /* a non-zero GIOErrorEnum code to complete with (not G_IO_ERROR_FAILED, which is 0) */
  gboolean      cancel;    /* cancel the operation's cancellable first */
  guint8        request;   /* CONTROL */
  guint16       value;
  guint16       index;
  guint         delay_ms;  /* WAIT: expected delay */
  guint         sleep_ms;  /* time that passes before completion */
  void        (*before) (gpointer data);  /* called first, with Fake.hook_data */
} FakeStep;

typedef struct
{
  const FakeStep *steps;
  gsize           n_steps;
  gsize           index;
  gchar          *mismatch;  /* first mismatch, or NULL */
  guint           pending;   /* operations not yet completed */
  gpointer        hook_data; /* passed to FakeStep.before */
} Fake;

extern const SidetouchTransportOps fake_ops;

void fake_init (Fake *fake, const FakeStep *steps, gsize n_steps);
void fake_clear (Fake *fake);

/* Shorthand for scripts. */
#define FAKE_OUT(bytes) { .kind = FAKE_SEND, .data = (bytes), .length = sizeof (bytes) }
#define FAKE_OUT_ANY { .kind = FAKE_SEND }
#define FAKE_IN(bytes) { .kind = FAKE_RECEIVE, .data = (bytes), .length = sizeof (bytes) }
