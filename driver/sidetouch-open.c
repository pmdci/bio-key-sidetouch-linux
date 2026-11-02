/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Pedro Innecco <https://pedroinnecco.com>
 */
/*
 * Opening the SideTouch reader. See sidetouch-open.h.
 */
#define FP_COMPONENT "sidetouch"

#include "sidetouch-open.h"
#include "sidetouch-bridge.h"
#include "sidetouch-exchange.h"
#include "sidetouch-proto.h"

typedef struct
{
  SidetouchTransport           transport;
  const SidetouchInterfaceOps *interface;
  gpointer                     interface_backend;
  GCancellable                *cancellable;
  guint16                     *next_sequence;
  guint16                      sequence;
  guint                        timeout_ms;
  SidetouchOpenDone            done;
  gpointer                     user_data;
} Open;

static void query (FpDevice *device, Open *self);

static void
finish (FpDevice *device, Open *self, GError *error)
{
  SidetouchOpenDone done = self->done;
  gpointer user_data = self->user_data;

  if (error)
    self->interface->release (self->interface_backend);
  g_clear_object (&self->cancellable);
  g_free (self);
  if (done)
    done (device, error, user_data);
  else
    g_clear_error (&error);
}

static void
query_done (FpDevice *device, const guint8 *payload, gsize length, GError *error, gpointer user_data)
{
  Open *self = user_data;
  uint8_t major = 0, minor = 0;

  if (g_error_matches (error, SIDETOUCH_EXCHANGE_ERROR, SIDETOUCH_EXCHANGE_ERROR_EMPTY_RESPONSE))
    {
      g_clear_error (&error);
      if (self->sequence == G_MAXUINT16)
        {
          finish (device, self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "SideTouch sequence space exhausted after empty responses"));
          return;
        }
      fp_dbg ("SideTouch sequence %u already used by the reader; trying the next", self->sequence);
      self->sequence++;
      query (device, self);
      return;
    }
  if (error)
    {
      finish (device, self, error);
      return;
    }
  if (!sidetouch_firmware_version (payload, length, &major, &minor))
    {
      finish (device, self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "SideTouch firmware query failed"));
      return;
    }
  if (major != SIDETOUCH_FIRMWARE_MAJOR || minor != SIDETOUCH_FIRMWARE_MINOR)
    fp_warn ("SideTouch firmware %u.%u is untested (only %u.%u has been tested); "
             "please report whether it works at https://github.com/pmdci/bio-key-sidetouch-linux/issues",
             major, minor, SIDETOUCH_FIRMWARE_MAJOR, SIDETOUCH_FIRMWARE_MINOR);
  else
    fp_info ("SideTouch firmware %u.%u", major, minor);
  *self->next_sequence = (guint16) (self->sequence + 1);
  finish (device, self, NULL);
}

static void
query (FpDevice *device, Open *self)
{
  sidetouch_exchange_async (device, &self->transport, self->sequence, sidetouch_cmd_info_b,
                            sizeof sidetouch_cmd_info_b, self->timeout_ms, self->cancellable, query_done, self);
}

static void
bridge_done (FpDevice *device, GError *error, gpointer user_data)
{
  Open *self = user_data;

  if (error)
    finish (device, self, error);
  else
    query (device, self);
}

void
sidetouch_open_async (FpDevice *device, const SidetouchTransport *transport, const SidetouchInterfaceOps *interface,
                      gpointer interface_backend, guint16 *next_sequence, guint timeout_ms, GCancellable *cancellable,
                      SidetouchOpenDone done, gpointer user_data)
{
  GError *error = NULL;
  Open *self;

  g_return_if_fail (FP_IS_DEVICE (device));
  g_return_if_fail (transport && transport->ops && interface && interface->claim && interface->release);
  g_return_if_fail (next_sequence);

  if (!interface->claim (interface_backend, &error))
    {
      /* nothing was claimed, so nothing is released */
      if (!error)
        error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "SideTouch interface claim failed");
      if (done)
        done (device, error, user_data);
      else
        g_clear_error (&error);
      return;
    }
  self = g_new0 (Open, 1);
  self->transport = *transport;
  self->interface = interface;
  self->interface_backend = interface_backend;
  self->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  self->next_sequence = next_sequence;
  self->sequence = *next_sequence;
  self->timeout_ms = timeout_ms;
  self->done = done;
  self->user_data = user_data;
  sidetouch_bridge_init_async (device, &self->transport, timeout_ms, self->cancellable, bridge_done, self);
}
