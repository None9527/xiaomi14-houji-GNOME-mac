/*
 * xs-audio: a PipeWire audio output for the client. GNOME lists it with the
 * other outputs (Settings > Sound) and, being the most preferred, plays
 * through it while the client is connected; what is played comes back as
 * interleaved 16-bit PCM.
 */
#pragma once

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#define XS_AUDIO_RATE 48000
#define XS_AUDIO_CHANNELS 2

typedef struct _XsAudio XsAudio;

/* Called on the audio thread with `frames` frames of PCM. */
typedef void (*XsAudioFunc) (const int16_t *pcm, size_t frames, int64_t pts_us,
                             void *user_data);

/* `description`: the output's name in GNOME Settings. */
XsAudio *xs_audio_new (const char *description, XsAudioFunc func, void *user_data,
                       GError **error);
void xs_audio_free (XsAudio *audio);
