#include "xs-audio.h"

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <time.h>

struct _XsAudio
{
  struct pw_thread_loop *loop;
  struct pw_stream *stream;
  struct spa_hook listener;
  XsAudioFunc func;
  void *user_data;
};

static void
on_process (void *data)
{
  XsAudio *audio = data;
  struct pw_buffer *b;
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  while ((b = pw_stream_dequeue_buffer (audio->stream)))
    {
      struct spa_data *d = &b->buffer->datas[0];
      uint32_t offset = SPA_MIN (d->chunk->offset, d->maxsize);
      uint32_t size = SPA_MIN (d->chunk->size, d->maxsize - offset);
      size_t frames = size / (sizeof (int16_t) * XS_AUDIO_CHANNELS);

      if (d->data && frames)
        audio->func ((const int16_t *) ((uint8_t *) d->data + offset), frames,
                     ts.tv_sec * 1000000ll + ts.tv_nsec / 1000, audio->user_data);
      pw_stream_queue_buffer (audio->stream, b);
    }
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_process,
};

XsAudio *
xs_audio_new (const char *description, XsAudioFunc func, void *user_data, GError **error)
{
  XsAudio *audio = g_new0 (XsAudio, 1);
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT (buffer, sizeof buffer);
  const struct spa_pod *params[1];
  struct pw_properties *props;

  pw_init (NULL, NULL);
  audio->func = func;
  audio->user_data = user_data;
  audio->loop = pw_thread_loop_new ("xs-audio", NULL);
  if (!audio->loop)
    goto fail;

  /* A sink, not a stream into one: applications play into it. 10 ms
   * quanta keep the added latency small. */
  props = pw_properties_new (PW_KEY_MEDIA_CLASS, "Audio/Sink",
                             PW_KEY_NODE_NAME, "xscreen",
                             PW_KEY_NODE_DESCRIPTION, description,
                             PW_KEY_NODE_LATENCY, "480/48000",
                             PW_KEY_PRIORITY_SESSION, "2000",
                             PW_KEY_PRIORITY_DRIVER, "2000",
                             NULL);
  audio->stream = pw_stream_new_simple (pw_thread_loop_get_loop (audio->loop), "xscreen",
                                        props, &stream_events, audio);
  if (!audio->stream)
    goto fail;

  params[0] = spa_format_audio_raw_build (&b, SPA_PARAM_EnumFormat,
    &SPA_AUDIO_INFO_RAW_INIT (.format = SPA_AUDIO_FORMAT_S16_LE,
                              .rate = XS_AUDIO_RATE,
                              .channels = XS_AUDIO_CHANNELS,
                              .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
  if (pw_stream_connect (audio->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                         PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0)
    goto fail;
  if (pw_thread_loop_start (audio->loop) < 0)
    goto fail;
  return audio;

fail:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "PipeWire audio output unavailable");
  xs_audio_free (audio);
  return NULL;
}

void
xs_audio_free (XsAudio *audio)
{
  if (!audio)
    return;
  if (audio->loop)
    pw_thread_loop_stop (audio->loop);
  if (audio->stream)
    pw_stream_destroy (audio->stream);
  if (audio->loop)
    pw_thread_loop_destroy (audio->loop);
  g_free (audio);
}
