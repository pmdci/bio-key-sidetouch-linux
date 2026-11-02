/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Scripted transport for the SideTouch state-machine tests. See sidetouch-fake.h.
 */
#include "sidetouch-fake.h"

#include <string.h>

typedef struct
{
  Fake                 *fake;
  FakeKind              kind;
  const guint8         *data;
  gsize                 length;
  guint8                request;
  guint16               value;
  guint16               index;
  guint                 delay_ms;
  GCancellable         *cancellable;
  SidetouchTransferDone done;
  gpointer              user_data;
} Pending;

static const char *const kind_names[] = { "send", "receive", "control", "recover", "wait" };

static void
note_mismatch (Fake *fake, gchar *message)
{
  if (!fake->mismatch)
    fake->mismatch = message;
  else
    g_free (message);
}

static gboolean
complete (gpointer user_data)
{
  Pending *p = user_data;
  Fake *fake = p->fake;
  const FakeStep *step = NULL;
  GError *error = NULL;
  const guint8 *reply = NULL;
  gsize reply_length = 0;

  if (fake->index >= fake->n_steps)
    {
      note_mismatch (fake, g_strdup_printf ("%s past the end of the script (step %" G_GSIZE_FORMAT ")",
                                            kind_names[p->kind], fake->index));
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "fake: script exhausted");
    }
  else
    {
      step = &fake->steps[fake->index++];
      if (g_getenv ("FAKE_TRACE"))
        g_printerr ("fake %3" G_GSIZE_FORMAT ": %-7s got %-7s len %3" G_GSIZE_FORMAT " first %02x\n",
                    fake->index - 1, kind_names[step->kind], kind_names[p->kind],
                    step->length, step->data && step->length ? step->data[step->length > 2 ? 2 : 0] : 0);
      if (step->sleep_ms)
        g_usleep ((gulong) step->sleep_ms * 1000);
      if (step->before)
        step->before (fake->hook_data);
      if (step->cancel && p->cancellable)
        g_cancellable_cancel (p->cancellable);

      if (step->kind != p->kind)
        note_mismatch (fake, g_strdup_printf ("step %" G_GSIZE_FORMAT ": expected %s, got %s",
                                              fake->index - 1, kind_names[step->kind], kind_names[p->kind]));
      else if (p->kind == FAKE_SEND && step->data &&
               (p->length != step->length || memcmp (p->data, step->data, p->length) != 0))
        note_mismatch (fake, g_strdup_printf ("step %" G_GSIZE_FORMAT ": sent %" G_GSIZE_FORMAT
                                              " bytes differ from the %" G_GSIZE_FORMAT " expected",
                                              fake->index - 1, p->length, step->length));
      else if (p->kind == FAKE_CONTROL &&
               (p->request != step->request || p->value != step->value || p->index != step->index))
        note_mismatch (fake, g_strdup_printf ("step %" G_GSIZE_FORMAT ": control %02x %04x %04x, expected %02x %04x %04x",
                                              fake->index - 1, p->request, p->value, p->index,
                                              step->request, step->value, step->index));
      else if (p->kind == FAKE_WAIT && p->delay_ms != step->delay_ms)
        note_mismatch (fake, g_strdup_printf ("step %" G_GSIZE_FORMAT ": wait %u ms, expected %u",
                                              fake->index - 1, p->delay_ms, step->delay_ms));

      if (p->cancellable && g_cancellable_is_cancelled (p->cancellable))
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "fake: cancelled");
      else if (step->error)
        error = g_error_new_literal (G_IO_ERROR, step->error, "fake: scripted error");
      else if (p->kind == FAKE_RECEIVE)
        {
          reply = step->data;
          reply_length = step->length;
        }
    }

  fake->pending--;
  p->done (reply, reply_length, error, p->user_data);
  g_clear_object (&p->cancellable);
  g_free ((gpointer) p->data);
  g_free (p);
  return G_SOURCE_REMOVE;
}

static void
submit (Fake *fake, FakeKind kind, const guint8 *data, gsize length, GCancellable *cancellable,
        SidetouchTransferDone done, gpointer user_data, Pending **out)
{
  Pending *p = g_new0 (Pending, 1);

  p->fake = fake;
  p->kind = kind;
  p->data = data ? g_memdup2 (data, length) : NULL;
  p->length = length;
  p->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  p->done = done;
  p->user_data = user_data;
  fake->pending++;
  if (out)
    *out = p;
  g_idle_add (complete, p);
}

static void
fake_send (gpointer backend, const guint8 *data, gsize length, guint timeout_ms,
           GCancellable *cancellable, SidetouchTransferDone done, gpointer user_data)
{
  (void) timeout_ms;
  submit (backend, FAKE_SEND, data, length, cancellable, done, user_data, NULL);
}

static void
fake_receive (gpointer backend, gsize capacity, guint timeout_ms, GCancellable *cancellable,
              SidetouchTransferDone done, gpointer user_data)
{
  (void) capacity;
  (void) timeout_ms;
  submit (backend, FAKE_RECEIVE, NULL, 0, cancellable, done, user_data, NULL);
}

static void
fake_control (gpointer backend, guint8 request, guint16 value, guint16 index, guint timeout_ms,
              GCancellable *cancellable, SidetouchTransferDone done, gpointer user_data)
{
  Pending *p;

  (void) timeout_ms;
  submit (backend, FAKE_CONTROL, NULL, 0, cancellable, done, user_data, &p);
  p->request = request;
  p->value = value;
  p->index = index;
}

static void
fake_recover (gpointer backend, GCancellable *cancellable, SidetouchTransferDone done, gpointer user_data)
{
  submit (backend, FAKE_RECOVER, NULL, 0, cancellable, done, user_data, NULL);
}

static void
fake_wait (gpointer backend, guint delay_ms, GCancellable *cancellable, SidetouchTransferDone done,
           gpointer user_data)
{
  Pending *p;

  submit (backend, FAKE_WAIT, NULL, 0, cancellable, done, user_data, &p);
  p->delay_ms = delay_ms;
}

const SidetouchTransportOps fake_ops = {
  .bulk_send = fake_send,
  .bulk_receive = fake_receive,
  .control_out = fake_control,
  .recover_in = fake_recover,
  .wait = fake_wait,
};

void
fake_init (Fake *fake, const FakeStep *steps, gsize n_steps)
{
  memset (fake, 0, sizeof *fake);
  fake->steps = steps;
  fake->n_steps = n_steps;
}

void
fake_clear (Fake *fake)
{
  g_clear_pointer (&fake->mismatch, g_free);
}
