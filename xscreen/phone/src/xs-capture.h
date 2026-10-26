/*
 * xs-capture: a mutter virtual monitor (org.gnome.Mutter.ScreenCast
 * RecordVirtual) delivered as dmabufs over PipeWire.
 */
#pragma once

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

#include "xs-gpu.h"

typedef struct _XsCapture XsCapture;

typedef struct
{
  int x;
  int y;
  int hotspot_x;
  int hotspot_y;
  bool visible;
  /* Set only when the sprite changed: RGBA, premultiplied, tightly packed. */
  const uint8_t *bitmap;
  int bitmap_width;
  int bitmap_height;
} XsCursorInfo;

typedef struct
{
  /* Called on the PipeWire thread with a frame that stays valid until the
   * callback returns. `damage` is what changed since the previous frame
   * delivered to this callback (full when unknown). */
  void (*frame) (const XsGpuSource *source,
                 const XsRegion    *damage,
                 int64_t            pts_us,
                 void              *user_data);
  /* Called on the PipeWire thread whenever cursor metadata arrives. */
  void (*cursor) (const XsCursorInfo *cursor, void *user_data);
  /* Called on the PipeWire thread when the negotiated size changes. */
  void (*resized) (int width, int height, void *user_data);
  /* Called on the PipeWire thread when stream buffers go away; imports of
   * their dmabufs must be dropped (fd numbers get reused). */
  void (*buffers_changed) (void *user_data);
  void *user_data;
} XsCaptureCallbacks;

/* `remote_desktop_session_id` may be NULL; when set, the screen cast is
 * linked to that RemoteDesktop session so input can target the stream. */
#define XS_MONITOR_MAX_MODES 96

typedef struct
{
  int width;
  int height;
  double refresh_rate;
  double preferred_scale;
  bool preferred;
} XsMonitorMode;

/* The virtual monitor as GNOME sees it: a stable identity, so GNOME
 * Settings remembers its position, mode and scale, and the modes it offers. */
typedef struct
{
  const char *vendor;
  const char *product;
  const char *serial;
  int n_modes;
  XsMonitorMode modes[XS_MONITOR_MAX_MODES];
} XsMonitorSpec;

/* `remote_desktop_session_id` may be NULL; when set, the screen cast is
 * linked to that RemoteDesktop session so input can target the stream.
 * A NULL `spec` makes a monitor that simply follows the stream size.
 * `embed_cursor` draws the pointer into the frames instead of delivering it
 * as metadata (for receivers that cannot draw it themselves). */
XsCapture *xs_capture_new (GDBusConnection          *bus,
                           const char               *remote_desktop_session_id,
                           const XsMonitorSpec      *spec,
                           int                       width,
                           int                       height,
                           int                       max_fps,
                           bool                      embed_cursor,
                           const XsCaptureCallbacks *callbacks,
                           GError                  **error);

void xs_capture_free (XsCapture *capture);

/* Path of the ScreenCast stream object, used by RemoteDesktop absolute
 * pointer events. */
const char *xs_capture_get_stream_path (XsCapture *capture);

/* Starts the session (or, for a linked capture, expects the caller to
 * start the RemoteDesktop session) and connects to the PipeWire node. */
bool xs_capture_start (XsCapture  *capture,
                       bool        start_session,
                       GError    **error);

/* Runs `func` on the PipeWire thread and waits for it. */
void xs_capture_invoke (XsCapture *capture,
                        void     (*func) (void *data),
                        void      *data);
