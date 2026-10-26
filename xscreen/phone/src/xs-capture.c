#include "xs-capture.h"

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC "org.gnome.Mutter.ScreenCast"
/* mutter offers cursor metadata of exactly this size, nothing else. */
#define CURSOR_MAX 384
#define DAMAGE_MAX_REGIONS 16
#define CURSOR_META_SIZE \
  (sizeof (struct spa_meta_cursor) + sizeof (struct spa_meta_bitmap) + CURSOR_MAX * CURSOR_MAX * 4)

struct _XsCapture
{
  GDBusConnection *bus;
  char *session_path;
  char *stream_path;
  guint stream_added_id;
  guint32 node_id;
  bool have_node;

  XsCaptureCallbacks callbacks;
  int width;
  int height;
  int max_fps;

  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook stream_listener;

  struct spa_video_info_raw format;
  bool format_has_modifier;

  uint8_t *cursor_rgba;
  uint32_t last_cursor_id;

  /* Damage of frames dequeued but not yet delivered. */
  XsRegion pending_damage;
};

static const struct spa_pod *
build_format (XsCapture *capture, struct spa_pod_builder *b, bool with_modifier)
{
  struct spa_pod_frame f[2];

  spa_pod_builder_push_object (b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add (b,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_BGRx),
    /* With modes, GNOME picks the size (GNOME Settings); follow it. */
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle (
      &SPA_RECTANGLE (capture->width, capture->height),
      &SPA_RECTANGLE (64, 64), &SPA_RECTANGLE (8192, 8192)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&SPA_FRACTION (0, 1)),
    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction (
      &SPA_FRACTION (capture->max_fps, 1), &SPA_FRACTION (1, 1),
      &SPA_FRACTION (capture->max_fps, 1)),
    0);
  if (with_modifier)
    {
      /* Linear only: both the GPU import and the NV12 pass expect it, and
       * mutter offers it on the anland backend. */
      spa_pod_builder_prop (b, SPA_FORMAT_VIDEO_modifier,
                            SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
      spa_pod_builder_push_choice (b, &f[1], SPA_CHOICE_Enum, 0);
      spa_pod_builder_long (b, DRM_FORMAT_MOD_LINEAR);
      spa_pod_builder_long (b, DRM_FORMAT_MOD_LINEAR);
      spa_pod_builder_pop (b, &f[1]);
    }
  return spa_pod_builder_pop (b, &f[0]);
}

static void
on_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  XsCapture *capture = data;
  uint8_t buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buf, sizeof buf);
  const struct spa_pod *params[4];

  if (!param || id != SPA_PARAM_Format)
    return;

  memset (&capture->format, 0, sizeof capture->format);
  spa_format_video_raw_parse (param, &capture->format);
  capture->format_has_modifier =
    spa_pod_find_prop (param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
  if (!capture->format_has_modifier)
    {
      fprintf (stderr, "xs-capture: stream negotiated without dmabuf; refusing\n");
      return;
    }

  params[0] = spa_pod_builder_add_object (&b,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (4, 3, 8),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int (1 << SPA_DATA_DmaBuf));
  params[1] = spa_pod_builder_add_object (&b,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
    SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_header)));
  params[2] = spa_pod_builder_add_object (&b,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Cursor),
    SPA_PARAM_META_size, SPA_POD_Int (CURSOR_META_SIZE));
  params[3] = spa_pod_builder_add_object (&b,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_VideoDamage),
    SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int (
      sizeof (struct spa_meta_region) * DAMAGE_MAX_REGIONS,
      sizeof (struct spa_meta_region) * 1,
      sizeof (struct spa_meta_region) * DAMAGE_MAX_REGIONS));
  pw_stream_update_params (capture->stream, params, 4);

  /* A new format means new buffers: everything is damaged. */
  xs_region_set_full (&capture->pending_damage);

  if (capture->callbacks.resized)
    capture->callbacks.resized (capture->format.size.width,
                                capture->format.size.height,
                                capture->callbacks.user_data);
}

static void
process_cursor (XsCapture *capture, struct spa_buffer *buffer)
{
  struct spa_meta_cursor *mc;
  XsCursorInfo info = { 0 };

  if (!capture->callbacks.cursor)
    return;
  mc = spa_buffer_find_meta_data (buffer, SPA_META_Cursor, sizeof *mc);
  if (getenv ("XS_DEBUG"))
    {
      fprintf (stderr, "xs-capture: buffer metas:");
      for (uint32_t i = 0; i < buffer->n_metas; i++)
        fprintf (stderr, " %u/%u", buffer->metas[i].type, buffer->metas[i].size);
      fprintf (stderr, " | cursor %s id=%u pos=%d,%d\n",
               mc ? "present" : "absent", mc ? mc->id : 0,
               mc ? mc->position.x : 0, mc ? mc->position.y : 0);
    }
  if (!mc)
    return;

  info.visible = spa_meta_cursor_is_valid (mc);
  info.x = mc->position.x;
  info.y = mc->position.y;
  info.hotspot_x = mc->hotspot.x;
  info.hotspot_y = mc->hotspot.y;

  if (info.visible && mc->bitmap_offset >= sizeof *mc)
    {
      struct spa_meta_bitmap *bm = SPA_PTROFF (mc, mc->bitmap_offset, struct spa_meta_bitmap);

      if (bm->size.width > 0 && bm->size.height > 0 &&
          bm->size.width <= CURSOR_MAX && bm->size.height <= CURSOR_MAX)
        {
          const uint8_t *src = SPA_PTROFF (bm, bm->offset, const uint8_t);
          bool bgra = bm->format == SPA_VIDEO_FORMAT_BGRA;

          for (uint32_t y = 0; y < bm->size.height; y++)
            {
              const uint8_t *s = src + y * bm->stride;
              uint8_t *d = capture->cursor_rgba + y * bm->size.width * 4;

              for (uint32_t x = 0; x < bm->size.width; x++, s += 4, d += 4)
                {
                  d[0] = bgra ? s[2] : s[0];
                  d[1] = s[1];
                  d[2] = bgra ? s[0] : s[2];
                  d[3] = s[3];
                }
            }
          info.bitmap = capture->cursor_rgba;
          info.bitmap_width = bm->size.width;
          info.bitmap_height = bm->size.height;
        }
    }

  capture->callbacks.cursor (&info, capture->callbacks.user_data);
}

static void
accumulate_damage (XsCapture *capture, struct spa_buffer *buffer)
{
  struct spa_meta *meta = spa_buffer_find_meta (buffer, SPA_META_VideoDamage);
  struct spa_meta_region *region;
  int width = capture->format.size.width;
  int height = capture->format.size.height;

  if (!meta)
    {
      xs_region_set_full (&capture->pending_damage);
      return;
    }

  spa_meta_for_each (region, meta)
    {
      XsRect rect;

      if (!spa_meta_region_is_valid (region))
        break;
      rect = (XsRect) {
        region->region.position.x, region->region.position.y,
        (int) region->region.size.width, (int) region->region.size.height,
      };
      xs_region_add (&capture->pending_damage, &rect, width, height);
    }
}

static void
on_process (void *data)
{
  XsCapture *capture = data;
  struct pw_buffer *frame = NULL;
  struct pw_buffer *b;

  /* Keep only the newest frame; cursor metadata is processed for each. */
  while ((b = pw_stream_dequeue_buffer (capture->stream)))
    {
      struct spa_meta_header *h =
        spa_buffer_find_meta_data (b->buffer, SPA_META_Header, sizeof *h);
      struct spa_data *d = &b->buffer->datas[0];

      process_cursor (capture, b->buffer);

      if ((h && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED)) ||
          d->type != SPA_DATA_DmaBuf || d->chunk->size == 0 ||
          (d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED))
        {
          pw_stream_queue_buffer (capture->stream, b);
          continue;
        }

      accumulate_damage (capture, b->buffer);
      if (frame)
        pw_stream_queue_buffer (capture->stream, frame);
      frame = b;
    }

  if (!frame)
    return;

  if (capture->callbacks.frame)
    {
      struct spa_data *d = &frame->buffer->datas[0];
      struct spa_meta_header *h =
        spa_buffer_find_meta_data (frame->buffer, SPA_META_Header, sizeof *h);
      XsGpuSource source = {
        .fd = d->fd,
        .drm_format = capture->format.format == SPA_VIDEO_FORMAT_BGRA ?
                      DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
        .modifier = capture->format.modifier,
        .width = capture->format.size.width,
        .height = capture->format.size.height,
        .offset = d->chunk->offset,
        .stride = d->chunk->stride,
      };
      int64_t pts_us = h ? h->pts / 1000 : (int64_t) (g_get_monotonic_time ());

      capture->callbacks.frame (&source, &capture->pending_damage, pts_us,
                                capture->callbacks.user_data);
    }

  xs_region_clear (&capture->pending_damage);
  pw_stream_queue_buffer (capture->stream, frame);
}

static void
on_state_changed (void *data, enum pw_stream_state old,
                  enum pw_stream_state state, const char *error)
{
  fprintf (stderr, "xs-capture: stream %s%s%s\n", pw_stream_state_as_string (state),
           error ? ": " : "", error ? error : "");
}

static void
on_remove_buffer (void *data, struct pw_buffer *buffer)
{
  XsCapture *capture = data;

  if (capture->callbacks.buffers_changed)
    capture->callbacks.buffers_changed (capture->callbacks.user_data);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state_changed,
  .remove_buffer = on_remove_buffer,
  .param_changed = on_param_changed,
  .process = on_process,
};

static void
on_stream_added (GDBusConnection *connection, const char *sender, const char *path,
                 const char *iface, const char *signal, GVariant *params,
                 gpointer user_data)
{
  XsCapture *capture = user_data;

  g_variant_get (params, "(u)", &capture->node_id);
  capture->have_node = true;
}

XsCapture *
xs_capture_new (GDBusConnection          *bus,
                const char               *remote_desktop_session_id,
                const XsMonitorSpec      *spec,
                int                       width,
                int                       height,
                int                       max_fps,
                bool                      embed_cursor,
                const XsCaptureCallbacks *callbacks,
                GError                  **error)
{
  XsCapture *capture = g_new0 (XsCapture, 1);
  GVariantBuilder opts;
  GVariant *reply;

  capture->bus = g_object_ref (bus);
  capture->callbacks = *callbacks;
  capture->width = width;
  capture->height = height;
  capture->max_fps = max_fps;
  capture->cursor_rgba = g_malloc (CURSOR_MAX * CURSOR_MAX * 4);
  xs_region_set_full (&capture->pending_damage);

  g_variant_builder_init (&opts, G_VARIANT_TYPE ("a{sv}"));
  if (remote_desktop_session_id)
    g_variant_builder_add (&opts, "{sv}", "remote-desktop-session-id",
                           g_variant_new_string (remote_desktop_session_id));
  reply = g_dbus_connection_call_sync (bus, SC, "/org/gnome/Mutter/ScreenCast", SC,
                                       "CreateSession", g_variant_new ("(a{sv})", &opts),
                                       G_VARIANT_TYPE ("(o)"), 0, -1, NULL, error);
  if (!reply)
    goto fail;
  g_variant_get (reply, "(o)", &capture->session_path);
  g_variant_unref (reply);

  g_variant_builder_init (&opts, G_VARIANT_TYPE ("a{sv}"));
  /* Cursor as metadata (drawn by the receiver), or embedded in frames. */
  g_variant_builder_add (&opts, "{sv}", "cursor-mode", g_variant_new_uint32 (embed_cursor ? 1 : 2));
  g_variant_builder_add (&opts, "{sv}", "is-platform", g_variant_new_boolean (TRUE));
  if (spec)
    {
      GVariantBuilder modes;

      g_variant_builder_add (&opts, "{sv}", "monitor-vendor", g_variant_new_string (spec->vendor));
      g_variant_builder_add (&opts, "{sv}", "monitor-product", g_variant_new_string (spec->product));
      g_variant_builder_add (&opts, "{sv}", "monitor-serial", g_variant_new_string (spec->serial));

      g_variant_builder_init (&modes, G_VARIANT_TYPE ("aa{sv}"));
      for (int i = 0; i < spec->n_modes; i++)
        {
          const XsMonitorMode *m = &spec->modes[i];

          g_variant_builder_open (&modes, G_VARIANT_TYPE ("a{sv}"));
          g_variant_builder_add (&modes, "{sv}", "size",
                                 g_variant_new ("(uu)", (guint32) m->width, (guint32) m->height));
          g_variant_builder_add (&modes, "{sv}", "refresh-rate", g_variant_new_double (m->refresh_rate));
          g_variant_builder_add (&modes, "{sv}", "is-preferred", g_variant_new_boolean (m->preferred));
          if (m->preferred_scale > 0)
            g_variant_builder_add (&modes, "{sv}", "preferred-scale",
                                   g_variant_new_double (m->preferred_scale));
          g_variant_builder_close (&modes);
        }
      g_variant_builder_add (&opts, "{sv}", "modes", g_variant_builder_end (&modes));
    }
  reply = g_dbus_connection_call_sync (bus, SC, capture->session_path, SC ".Session",
                                       "RecordVirtual", g_variant_new ("(a{sv})", &opts),
                                       G_VARIANT_TYPE ("(o)"), 0, -1, NULL, error);
  if (!reply)
    goto fail;
  g_variant_get (reply, "(o)", &capture->stream_path);
  g_variant_unref (reply);

  capture->stream_added_id =
    g_dbus_connection_signal_subscribe (bus, SC, SC ".Stream", "PipeWireStreamAdded",
                                        capture->stream_path, NULL, 0,
                                        on_stream_added, capture, NULL);
  return capture;

fail:
  xs_capture_free (capture);
  return NULL;
}

const char *
xs_capture_get_stream_path (XsCapture *capture)
{
  return capture->stream_path;
}

bool
xs_capture_start (XsCapture *capture, bool start_session, GError **error)
{
  uint8_t buf[2048];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buf, sizeof buf);
  const struct spa_pod *params[1];
  gint64 deadline;

  if (start_session &&
      !g_dbus_connection_call_sync (capture->bus, SC, capture->session_path,
                                    SC ".Session", "Start", NULL, NULL, 0, -1,
                                    NULL, error))
    return false;

  deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (!capture->have_node && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
  if (!capture->have_node)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                   "mutter did not announce a PipeWire node");
      return false;
    }

  pw_init (NULL, NULL);
  capture->loop = pw_thread_loop_new ("xs-capture", NULL);
  capture->context = pw_context_new (pw_thread_loop_get_loop (capture->loop), NULL, 0);
  pw_thread_loop_lock (capture->loop);
  pw_thread_loop_start (capture->loop);
  capture->core = pw_context_connect (capture->context, NULL, 0);
  if (!capture->core)
    {
      pw_thread_loop_unlock (capture->loop);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot connect to PipeWire");
      return false;
    }
  capture->stream = pw_stream_new (capture->core, "xscreen",
    pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                       PW_KEY_MEDIA_CATEGORY, "Capture",
                       PW_KEY_MEDIA_ROLE, "Screen", NULL));
  pw_stream_add_listener (capture->stream, &capture->stream_listener,
                          &stream_events, capture);
  params[0] = build_format (capture, &b, true);
  pw_stream_connect (capture->stream, PW_DIRECTION_INPUT, capture->node_id,
                     PW_STREAM_FLAG_AUTOCONNECT, params, 1);
  pw_thread_loop_unlock (capture->loop);
  return true;
}

typedef struct
{
  void (*func) (void *data);
  void *data;
} InvokeData;

static int
do_invoke (struct spa_loop *loop, bool async, uint32_t seq, const void *data,
           size_t size, void *user_data)
{
  const InvokeData *invoke = data;

  invoke->func (invoke->data);
  return 0;
}

void
xs_capture_invoke (XsCapture *capture, void (*func) (void *data), void *data)
{
  InvokeData invoke = { func, data };

  if (!capture->loop)
    return;
  pw_loop_invoke (pw_thread_loop_get_loop (capture->loop), do_invoke, 0,
                  &invoke, sizeof invoke, true, capture);
}

void
xs_capture_free (XsCapture *capture)
{
  if (!capture)
    return;

  if (capture->loop)
    {
      pw_thread_loop_lock (capture->loop);
      if (capture->stream)
        pw_stream_destroy (capture->stream);
      if (capture->core)
        pw_core_disconnect (capture->core);
      pw_thread_loop_unlock (capture->loop);
      pw_thread_loop_stop (capture->loop);
      if (capture->context)
        pw_context_destroy (capture->context);
      pw_thread_loop_destroy (capture->loop);
    }
  if (capture->stream_added_id)
    g_dbus_connection_signal_unsubscribe (capture->bus, capture->stream_added_id);
  if (capture->session_path)
    g_dbus_connection_call_sync (capture->bus, SC, capture->session_path,
                                 SC ".Session", "Stop", NULL, NULL, 0, -1, NULL, NULL);
  g_free (capture->session_path);
  g_free (capture->stream_path);
  g_free (capture->cursor_rgba);
  g_clear_object (&capture->bus);
  g_free (capture);
}
