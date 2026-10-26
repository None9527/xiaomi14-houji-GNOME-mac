/*
 * xs-scrcpy: the scrcpy wire protocol (client versions 4.0 and newer), so the
 * stock scrcpy client can show and drive the GNOME virtual monitor.
 *
 * Framing only; no I/O. Video: a 12-byte session header announcing the size,
 * then media packets with a 12-byte header (PTS and flags, size). Control:
 * messages from the client become XsInputEvents and requests through the
 * callbacks; device messages (clipboard, acks) are built for the caller to
 * send. See scrcpy's doc/develop.md and app/src/{demuxer,control_msg}.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../protocol/xs-protocol.h"

#define XS_SCRCPY_HEADER_SIZE 12
#define XS_SCRCPY_CODEC_DISABLED 0u
#define XS_SCRCPY_CODEC_ERROR 1u
#define XS_SCRCPY_CODEC_H265 0x68323635u   /* "h265" */

/* Video framing. */
void xs_scrcpy_session_header (uint8_t out[XS_SCRCPY_HEADER_SIZE], int width, int height);
void xs_scrcpy_packet_header (uint8_t out[XS_SCRCPY_HEADER_SIZE], int64_t pts_us,
                              bool config, bool keyframe, uint32_t size);

/* Length of the VPS/SPS/PPS NAL units leading an HEVC Annex-B access unit
 * (0 when it starts with a slice). */
size_t xs_scrcpy_parameter_sets_length (const uint8_t *access_unit, size_t size);

/* Device messages; return the length written to `out` (0 if it does not fit). */
size_t xs_scrcpy_clipboard_message (uint8_t *out, size_t out_size, const char *text);
size_t xs_scrcpy_ack_clipboard_message (uint8_t out[9], uint64_t sequence);

typedef struct
{
  void (*input) (const XsInputEvent *event, void *user_data);
  void (*keyframe) (void *user_data);
  /* copy_key: 0 none, 1 copy, 2 cut (to press before reading). */
  void (*get_clipboard) (int copy_key, void *user_data);
  /* Ack with `sequence` once set, when it is not 0. */
  void (*set_clipboard) (const char *text, bool paste, uint64_t sequence, void *user_data);
  void (*start_app) (const char *name, void *user_data);
  void *user_data;
} XsScrcpyCallbacks;

typedef struct _XsScrcpyControl XsScrcpyControl;

/* `width`/`height`: the stream size positions are relative to. */
XsScrcpyControl *xs_scrcpy_control_new (const XsScrcpyCallbacks *callbacks,
                                        int width, int height);
void xs_scrcpy_control_free (XsScrcpyControl *control);
void xs_scrcpy_control_set_size (XsScrcpyControl *control, int width, int height);

/* Parses complete messages from `data`; returns the bytes consumed, or -1
 * on a malformed stream. Incomplete trailing bytes are left unconsumed. */
long xs_scrcpy_control_feed (XsScrcpyControl *control, const uint8_t *data, size_t size);
