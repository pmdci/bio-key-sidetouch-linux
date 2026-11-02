/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * The SideTouch transport over USB. See sidetouch-usb.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch-usb.h"

#define RECOVERY_TIMEOUT_MS 1000

typedef struct
{
  SidetouchTransferDone done;
  gpointer              user_data;
} Completion;

static Completion *
completion_new (SidetouchTransferDone done, gpointer user_data)
{
  Completion *c = g_new (Completion, 1);

  c->done = done;
  c->user_data = user_data;
  return c;
}

static void
transfer_done (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  Completion *c = user_data;

  (void) device;
  if (error || transfer->endpoint != SIDETOUCH_EP_IN)
    c->done (NULL, 0, error, c->user_data);
  else
    c->done (transfer->buffer, (gsize) transfer->actual_length, NULL, c->user_data);
  g_free (c);
}

static void
usb_send (gpointer backend, const guint8 *data, gsize length, guint timeout_ms, GCancellable *cancellable,
          SidetouchTransferDone done, gpointer user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (backend));

  fpi_usb_transfer_fill_bulk_full (transfer, SIDETOUCH_EP_OUT, g_memdup2 (data, length), length, g_free);
  /* a command sent in part is an error, not a success */
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable, transfer_done, completion_new (done, user_data));
}

static void
usb_receive (gpointer backend, gsize capacity, guint timeout_ms, GCancellable *cancellable,
             SidetouchTransferDone done, gpointer user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (backend));

  fpi_usb_transfer_fill_bulk (transfer, SIDETOUCH_EP_IN, capacity);
  /* status, acknowledgement and length replies are deliberately short */
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable, transfer_done, completion_new (done, user_data));
}

static void
usb_control (gpointer backend, guint8 request, guint16 value, guint16 index, guint timeout_ms,
             GCancellable *cancellable, SidetouchTransferDone done, gpointer user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (backend));

  fpi_usb_transfer_fill_control (transfer, G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR, G_USB_DEVICE_RECIPIENT_DEVICE,
                                 request, value, index, 0);
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable, transfer_done, completion_new (done, user_data));
}

/* CLEAR_FEATURE(ENDPOINT_HALT) on the IN endpoint. */
static void
usb_recover_in (gpointer backend, GCancellable *cancellable, SidetouchTransferDone done, gpointer user_data)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (backend));

  fpi_usb_transfer_fill_control (transfer, G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_STANDARD, G_USB_DEVICE_RECIPIENT_ENDPOINT,
                                 0x01, 0x0000, SIDETOUCH_EP_IN, 0);
  fpi_usb_transfer_submit (transfer, RECOVERY_TIMEOUT_MS, cancellable, transfer_done,
                           completion_new (done, user_data));
}

/* A delay that a cancellation ends early. Both ends are main-context sources,
 * so whichever fires first removes the other. */
typedef struct
{
  GSource              *timeout;
  GSource              *cancelled;
  SidetouchTransferDone done;
  gpointer              user_data;
} Wait;

static void
wait_finish (Wait *w, GError *error)
{
  SidetouchTransferDone done = w->done;
  gpointer user_data = w->user_data;

  if (w->timeout)
    g_source_destroy (w->timeout);
  if (w->cancelled)
    {
      g_source_destroy (w->cancelled);
      g_source_unref (w->cancelled);
    }
  g_free (w);
  done (NULL, 0, error, user_data);
}

static void
wait_elapsed (FpDevice *device, gpointer user_data)
{
  Wait *w = user_data;

  (void) device;
  w->timeout = NULL;   /* the device removes its timeout source itself */
  wait_finish (w, NULL);
}

static gboolean
wait_cancelled (GCancellable *cancellable, gpointer user_data)
{
  (void) cancellable;
  wait_finish (user_data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "SideTouch delay cancelled"));
  return G_SOURCE_REMOVE;
}

static void
usb_wait (gpointer backend, guint delay_ms, GCancellable *cancellable, SidetouchTransferDone done,
          gpointer user_data)
{
  Wait *w = g_new0 (Wait, 1);

  w->done = done;
  w->user_data = user_data;
  w->timeout = fpi_device_add_timeout (FP_DEVICE (backend), (gint) delay_ms, wait_elapsed, w, NULL);
  if (cancellable)
    {
      w->cancelled = g_cancellable_source_new (cancellable);
      g_source_set_callback (w->cancelled, G_SOURCE_FUNC (wait_cancelled), w, NULL);
      g_source_attach (w->cancelled, g_main_context_get_thread_default ());
    }
}

const SidetouchTransportOps sidetouch_usb_ops = {
  .bulk_send = usb_send,
  .bulk_receive = usb_receive,
  .control_out = usb_control,
  .recover_in = usb_recover_in,
  .wait = usb_wait,
};
