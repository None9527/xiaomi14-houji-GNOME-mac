#include "xs-input.h"

#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <libei.h>
#include <stdio.h>
#include <stdlib.h>

#define RD "org.gnome.Mutter.RemoteDesktop"
#define SC "org.gnome.Mutter.ScreenCast"

/* org.gnome.Mutter.RemoteDesktop device types */
#define DEVICE_TYPE_KEYBOARD (1 << 0)
#define DEVICE_TYPE_POINTER (1 << 1)

typedef struct
{
  struct ei_device *device;
  bool emulating;
} Device;

struct _XsInput
{
  struct ei *ei;
  guint source_id;
  uint32_t sequence;
  char *mapping_id;

  Device absolute;       /* mapped to our virtual monitor */
  Device relative;
  Device keyboard;
  Device *last_pointer;  /* receives buttons and scroll */

  XsInputRegionFunc region_func;
  void *region_data;

  /* Region of the absolute device, in compositor logical coordinates. */
  double region_x;
  double region_y;
  double region_scale;
};

static void
device_clear (Device *d)
{
  g_clear_pointer (&d->device, ei_device_unref);
  d->emulating = false;
}

static Device *
slot_for (XsInput *input, struct ei_device *device)
{
  if (input->absolute.device == device)
    return &input->absolute;
  if (input->relative.device == device)
    return &input->relative;
  if (input->keyboard.device == device)
    return &input->keyboard;
  return NULL;
}

/* Picks the absolute device whose region belongs to our stream. */
static bool
match_absolute (XsInput *input, struct ei_device *device)
{
  for (size_t i = 0;; i++)
    {
      struct ei_region *region = ei_device_get_region (device, i);
      const char *mapping_id;

      if (!region)
        return false;
      mapping_id = ei_region_get_mapping_id (region);
      if (input->mapping_id && g_strcmp0 (mapping_id, input->mapping_id) != 0)
        continue;

      input->region_x = ei_region_get_x (region);
      input->region_y = ei_region_get_y (region);
      input->region_scale = ei_region_get_physical_scale (region);
      if (input->region_scale <= 0)
        input->region_scale = 1;
      fprintf (stderr, "xs-input: virtual monitor at %.0f,%.0f, %ux%u logical, scale %.2f\n",
               input->region_x, input->region_y, ei_region_get_width (region),
               ei_region_get_height (region), input->region_scale);
      if (input->region_func)
        input->region_func (input->region_x, input->region_y,
                            ei_region_get_width (region), ei_region_get_height (region),
                            input->region_data);
      return true;
    }
}

static bool
debug_enabled (void)
{
  static int enabled = -1;

  if (enabled < 0)
    enabled = getenv ("XS_DEBUG") != NULL;
  return enabled;
}

static void
on_device_added (XsInput *input, struct ei_device *device)
{
  if (debug_enabled ())
    fprintf (stderr, "xs-input: device '%s' abs=%d rel=%d kbd=%d\n", ei_device_get_name (device),
             ei_device_has_capability (device, EI_DEVICE_CAP_POINTER_ABSOLUTE),
             ei_device_has_capability (device, EI_DEVICE_CAP_POINTER),
             ei_device_has_capability (device, EI_DEVICE_CAP_KEYBOARD));
  if (ei_device_has_capability (device, EI_DEVICE_CAP_POINTER_ABSOLUTE))
    {
      if (!input->absolute.device && match_absolute (input, device))
        input->absolute.device = ei_device_ref (device);
    }
  else if (ei_device_has_capability (device, EI_DEVICE_CAP_POINTER))
    {
      if (!input->relative.device)
        input->relative.device = ei_device_ref (device);
    }
  if (ei_device_has_capability (device, EI_DEVICE_CAP_KEYBOARD) && !input->keyboard.device)
    input->keyboard.device = ei_device_ref (device);
}

static gboolean
on_ei_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  XsInput *input = user_data;
  struct ei_event *event;

  ei_dispatch (input->ei);
  while ((event = ei_get_event (input->ei)))
    {
      struct ei_device *device = ei_event_get_device (event);
      Device *slot;

      switch (ei_event_get_type (event))
        {
        case EI_EVENT_SEAT_ADDED:
          ei_seat_bind_capabilities (ei_event_get_seat (event),
                                     EI_DEVICE_CAP_POINTER,
                                     EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                     EI_DEVICE_CAP_BUTTON,
                                     EI_DEVICE_CAP_SCROLL,
                                     EI_DEVICE_CAP_KEYBOARD,
                                     NULL);
          break;
        case EI_EVENT_DEVICE_ADDED:
          on_device_added (input, device);
          break;
        case EI_EVENT_DEVICE_RESUMED:
          slot = slot_for (input, device);
          if (slot && !slot->emulating)
            {
              ei_device_start_emulating (device, ++input->sequence);
              slot->emulating = true;
              if (debug_enabled ())
                fprintf (stderr, "xs-input: emulating on '%s'\n", ei_device_get_name (device));
            }
          break;
        case EI_EVENT_DEVICE_PAUSED:
          slot = slot_for (input, device);
          if (slot)
            slot->emulating = false;
          break;
        case EI_EVENT_DEVICE_REMOVED:
          slot = slot_for (input, device);
          if (slot)
            {
              if (input->last_pointer == slot)
                input->last_pointer = NULL;
              device_clear (slot);
            }
          break;
        case EI_EVENT_DISCONNECT:
          fprintf (stderr, "xs-input: EIS disconnected\n");
          break;
        default:
          break;
        }
      ei_event_unref (event);
    }
  return G_SOURCE_CONTINUE;
}

static char *
get_mapping_id (GDBusConnection *bus, const char *stream_path)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) parameters = NULL;
  const char *mapping_id = NULL;

  reply = g_dbus_connection_call_sync (bus, SC, stream_path,
                                       "org.freedesktop.DBus.Properties", "Get",
                                       g_variant_new ("(ss)", SC ".Stream", "Parameters"),
                                       G_VARIANT_TYPE ("(v)"), 0, -1, NULL, NULL);
  if (!reply)
    return NULL;
  g_variant_get (reply, "(v)", &parameters);
  g_variant_lookup (parameters, "mapping-id", "&s", &mapping_id);
  return g_strdup (mapping_id);
}

XsInput *
xs_input_new (GDBusConnection    *bus,
              const char         *rd_session_path,
              const char         *stream_path,
              XsInputRegionFunc   region_func,
              void               *user_data,
              GError            **error)
{
  XsInput *input = g_new0 (XsInput, 1);
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  GVariantBuilder options;
  gint32 fd_index;
  int fd;

  input->region_scale = 1;
  input->region_func = region_func;
  input->region_data = user_data;
  input->mapping_id = get_mapping_id (bus, stream_path);
  if (!input->mapping_id)
    fprintf (stderr, "xs-input: stream has no mapping id; using the first absolute device\n");

  g_variant_builder_init (&options, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&options, "{sv}", "device-types",
                         g_variant_new_uint32 (DEVICE_TYPE_KEYBOARD | DEVICE_TYPE_POINTER));
  reply = g_dbus_connection_call_with_unix_fd_list_sync (bus, RD, rd_session_path, RD ".Session",
                                                         "ConnectToEIS",
                                                         g_variant_new ("(a{sv})", &options),
                                                         G_VARIANT_TYPE ("(h)"), 0, -1,
                                                         NULL, &fd_list, NULL, error);
  if (!reply)
    goto fail;
  g_variant_get (reply, "(h)", &fd_index);
  fd = g_unix_fd_list_get (fd_list, fd_index, error);
  if (fd < 0)
    goto fail;

  input->ei = ei_new_sender (input);
  ei_configure_name (input->ei, "xscreen");
  if (ei_setup_backend_fd (input->ei, fd) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "libei setup failed");
      goto fail;
    }
  input->source_id = g_unix_fd_add (ei_get_fd (input->ei), G_IO_IN, on_ei_readable, input);
  return input;

fail:
  xs_input_free (input);
  return NULL;
}

void
xs_input_free (XsInput *input)
{
  if (!input)
    return;
  if (input->source_id)
    g_source_remove (input->source_id);
  device_clear (&input->absolute);
  device_clear (&input->relative);
  device_clear (&input->keyboard);
  g_clear_pointer (&input->ei, ei_unref);
  g_free (input->mapping_id);
  g_free (input);
}

static void
frame (XsInput *input, Device *d)
{
  ei_device_frame (d->device, ei_now (input->ei));
}

void
xs_input_handle (XsInput *input, const XsInputEvent *event)
{
  Device *pointer = input->last_pointer ? input->last_pointer : &input->absolute;

  switch (event->type)
    {
    case XS_INPUT_POINTER_ABSOLUTE:
      if (!input->absolute.emulating)
        {
          if (debug_enabled ())
            fprintf (stderr, "xs-input: dropped absolute motion (device %s)\n",
                     input->absolute.device ? "paused" : "missing");
          return;
        }
      ei_device_pointer_motion_absolute (input->absolute.device,
                                         input->region_x + event->x / input->region_scale,
                                         input->region_y + event->y / input->region_scale);
      frame (input, &input->absolute);
      input->last_pointer = &input->absolute;
      break;

    case XS_INPUT_POINTER_RELATIVE:
      if (!input->relative.emulating)
        {
          if (debug_enabled ())
            fprintf (stderr, "xs-input: dropped relative motion (device %s)\n",
                     input->relative.device ? "paused" : "missing");
          return;
        }
      ei_device_pointer_motion (input->relative.device,
                                event->x / input->region_scale,
                                event->y / input->region_scale);
      frame (input, &input->relative);
      input->last_pointer = &input->relative;
      break;

    case XS_INPUT_BUTTON:
      if (!pointer->emulating)
        return;
      ei_device_button_button (pointer->device, event->code, event->value != 0);
      frame (input, pointer);
      break;

    case XS_INPUT_SCROLL:
      if (!pointer->emulating)
        return;
      ei_device_scroll_delta (pointer->device, event->x / input->region_scale,
                              event->y / input->region_scale);
      frame (input, pointer);
      break;

    case XS_INPUT_SCROLL_DISCRETE:
      if (!pointer->emulating)
        return;
      /* libei discrete steps are in 1/120ths of a wheel detent. */
      if (event->code == 0)
        ei_device_scroll_discrete (pointer->device, 0, event->value * 120);
      else
        ei_device_scroll_discrete (pointer->device, event->value * 120, 0);
      frame (input, pointer);
      break;

    case XS_INPUT_KEY:
      if (!input->keyboard.emulating)
        {
          if (debug_enabled ())
            fprintf (stderr, "xs-input: dropped key %u (device %s)\n", event->code,
                     input->keyboard.device ? "paused" : "missing");
          return;
        }
      ei_device_keyboard_key (input->keyboard.device, event->code, event->value != 0);
      frame (input, &input->keyboard);
      break;

    default:
      break;
    }
}
