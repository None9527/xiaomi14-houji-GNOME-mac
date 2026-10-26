/*
 * pw-dmabuf-probe: create a mutter virtual monitor (ScreenCast.RecordVirtual)
 * and check that its PipeWire stream delivers DmaBuf buffers.
 *
 * build: gcc -O2 -o pw-dmabuf-probe pw-dmabuf-probe.c \
 *          $(pkg-config --cflags --libs libpipewire-0.3 gio-2.0)
 */
#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>
#include <drm_fourcc.h>
#include <stdio.h>

#define SC "org.gnome.Mutter.ScreenCast"

static struct
{
  struct pw_thread_loop *loop;
  struct pw_stream *stream;
  struct spa_hook listener;
  struct spa_video_info_raw info;
  uint64_t modifier;
  int frames;
  int width, height;
} probe;

static const struct spa_pod *
build_format (struct spa_pod_builder *b, gboolean with_modifier)
{
  struct spa_pod_frame f[2];
  uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR };

  spa_pod_builder_push_object (b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add (b,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_Id (SPA_VIDEO_FORMAT_BGRx),
    SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle (&SPA_RECTANGLE (probe.width, probe.height)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&SPA_FRACTION (0, 1)),
    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction (
      &SPA_FRACTION (60, 1), &SPA_FRACTION (1, 1), &SPA_FRACTION (120, 1)),
    0);
  if (with_modifier)
    {
      spa_pod_builder_prop (b, SPA_FORMAT_VIDEO_modifier,
                            SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
      spa_pod_builder_push_choice (b, &f[1], SPA_CHOICE_Enum, 0);
      spa_pod_builder_long (b, mods[0]);
      spa_pod_builder_long (b, mods[1]);
      spa_pod_builder_pop (b, &f[1]);
    }
  return spa_pod_builder_pop (b, &f[0]);
}

static void
on_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  uint8_t buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buf, sizeof buf);
  const struct spa_pod *params[1];
  const struct spa_pod_prop *mod_prop;

  if (!param || id != SPA_PARAM_Format)
    return;
  spa_format_video_raw_parse (param, &probe.info);
  mod_prop = spa_pod_find_prop (param, NULL, SPA_FORMAT_VIDEO_modifier);
  probe.modifier = probe.info.modifier;
  printf ("format: %s %ux%u modifier=%s0x%llx\n",
          spa_debug_type_find_name (spa_type_video_format, probe.info.format),
          probe.info.size.width, probe.info.size.height,
          mod_prop ? "" : "(none) ", (unsigned long long) probe.modifier);

  params[0] = spa_pod_builder_add_object (&b,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int (4, 2, 8),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int (mod_prop ? (1 << SPA_DATA_DmaBuf)
                                                      : (1 << SPA_DATA_MemFd)));
  pw_stream_update_params (probe.stream, params, 1);
}

static void
on_process (void *data)
{
  struct pw_buffer *b = pw_stream_dequeue_buffer (probe.stream);
  struct spa_data *d;

  if (!b)
    return;
  d = &b->buffer->datas[0];
  if (d->chunk->size > 0 || d->type == SPA_DATA_DmaBuf)
    {
      printf ("frame %d: type=%s fd=%lld planes=%u stride=%d offset=%u size=%u\n",
              probe.frames, d->type == SPA_DATA_DmaBuf ? "DmaBuf" :
                            d->type == SPA_DATA_MemFd ? "MemFd" : "other",
              (long long) d->fd, b->buffer->n_datas, d->chunk->stride,
              d->chunk->offset, d->chunk->size);
      probe.frames++;
    }
  pw_stream_queue_buffer (probe.stream, b);
  if (probe.frames >= 5)
    pw_thread_loop_signal (probe.loop, FALSE);
}

static void
on_state (void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  printf ("state: %s %s\n", pw_stream_state_as_string (state), error ? error : "");
  if (state == PW_STREAM_STATE_ERROR)
    pw_thread_loop_signal (probe.loop, FALSE);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_state,
  .param_changed = on_param_changed,
  .process = on_process,
};

static guint32 node_id;

static void
on_stream_added (GDBusConnection *c, const char *s, const char *p, const char *i,
                 const char *n, GVariant *params, gpointer loop)
{
  g_variant_get (params, "(u)", &node_id);
  g_main_loop_quit (loop);
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_autofree char *session = NULL, *stream = NULL;
  GVariantBuilder opts;
  GVariant *r;
  uint8_t buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buf, sizeof buf);
  const struct spa_pod *params[2];
  gboolean with_modifier = argc < 2 || g_strcmp0 (argv[1], "memfd") != 0;

  probe.width = 1920;
  probe.height = 1080;

  r = g_dbus_connection_call_sync (bus, SC, "/org/gnome/Mutter/ScreenCast", SC,
        "CreateSession", g_variant_new ("(a{sv})", NULL), G_VARIANT_TYPE ("(o)"),
        0, -1, NULL, &error);
  if (!r) { g_printerr ("CreateSession: %s\n", error->message); return 1; }
  g_variant_get (r, "(o)", &session);

  g_variant_builder_init (&opts, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&opts, "{sv}", "cursor-mode", g_variant_new_uint32 (2));
  g_variant_builder_add (&opts, "{sv}", "is-platform", g_variant_new_boolean (TRUE));
  r = g_dbus_connection_call_sync (bus, SC, session, SC ".Session", "RecordVirtual",
        g_variant_new ("(a{sv})", &opts), G_VARIANT_TYPE ("(o)"), 0, -1, NULL, &error);
  if (!r) { g_printerr ("RecordVirtual: %s\n", error->message); return 1; }
  g_variant_get (r, "(o)", &stream);

  g_dbus_connection_signal_subscribe (bus, SC, SC ".Stream", "PipeWireStreamAdded",
                                      stream, NULL, 0, on_stream_added, loop, NULL);
  g_dbus_connection_call_sync (bus, SC, session, SC ".Session", "Start", NULL, NULL,
                               0, -1, NULL, &error);
  g_main_loop_run (loop);
  printf ("node %u (modifier negotiation %s)\n", node_id, with_modifier ? "on" : "off");

  pw_init (&argc, &argv);
  probe.loop = pw_thread_loop_new ("probe", NULL);
  pw_thread_loop_lock (probe.loop);
  pw_thread_loop_start (probe.loop);
  probe.stream = pw_stream_new_simple (pw_thread_loop_get_loop (probe.loop), "xscreen-probe",
      pw_properties_new (PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", NULL),
      &stream_events, NULL);
  params[0] = build_format (&b, with_modifier);
  params[1] = build_format (&b, FALSE);
  pw_stream_connect (probe.stream, PW_DIRECTION_INPUT, node_id,
                     PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                     params, with_modifier ? 2 : 1);
  {
    struct timespec ts;
    pw_thread_loop_get_time (probe.loop, &ts, 8 * SPA_NSEC_PER_SEC);
    pw_thread_loop_timed_wait_full (probe.loop, &ts);
  }
  printf ("received %d frames\n", probe.frames);
  pw_stream_destroy (probe.stream);
  pw_thread_loop_unlock (probe.loop);

  g_dbus_connection_call_sync (bus, SC, session, SC ".Session", "Stop", NULL, NULL,
                               0, -1, NULL, NULL);
  return probe.frames > 0 ? 0 : 2;
}
