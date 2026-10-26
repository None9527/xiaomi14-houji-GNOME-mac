/*
 * xscreend: extends the GNOME desktop onto a remote display over USB.
 *
 * While a client is connected, a mutter virtual monitor exists. Its frames
 * arrive as dmabufs, the GPU converts only what changed into NV12 encoder
 * buffers, the VPU encodes HEVC, and access units go to the client over an
 * adb-forwarded socket. A static picture gets one extra quality pass.
 *
 * usage: xscreend [--port N] [--raw WIDTHxHEIGHT]
 *   --raw: no protocol, writes a bare Annex-B stream to the first client
 *          (for testing with `ffplay -f hevc tcp://127.0.0.1:N`).
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <math.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/dma-buf.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

#include "xs-audio.h"
#include "xs-capture.h"
#include "xs-clipboard.h"
#include "xs-damage.h"
#include "xs-enc.h"
#include "xs-files.h"
#include "xs-gpu.h"
#include "xs-input.h"
#include "xs-layout.h"
#include "xs-scrcpy.h"
#include "../../protocol/xs-protocol.h"

#define RD "org.gnome.Mutter.RemoteDesktop"
#define ENCODER_DEVICE "/dev/video33"
#define MAX_FPS 60
#define REFINE_DELAY_US 150000
/* A refinement keyframe costs ~1 MB; small updates (a blinking caret, a
 * clock, a typed character) are sharp enough as P frames. */
#define REFINE_MIN_DAMAGE 0.05
/* P frames carry motion: bounded so it stays legible. I frames only come at
 * the start and once a picture is still, so they get near-transparent
 * quality (about 40 dB luma on detailed content, ~1 MB at 2560x1600). */
#define I_FRAME_MAX_QP 18
#define P_FRAME_MAX_QP 30

typedef struct
{
  GDBusConnection *bus;
  int port;
  bool raw;
  int raw_width;
  int raw_height;

  /* Client connection; writes come from several threads. For scrcpy it is
   * the video stream. */
  int client_fd;
  pthread_mutex_t write_lock;
  GSource *client_source;
  uint8_t *read_buffer;       /* grows to the largest message */
  size_t read_capacity;
  size_t read_fill;

  /* xscreen client extras. */
  XsAudio *audio;
  XsFileSender *file_sender;
  XsFileReceiver *file_receiver;
  guint32 download_notification;

  /* scrcpy client (instead of the xscreen one): its other streams. */
  bool scrcpy;
  bool scrcpy_cursor;         /* draw the pointer into the picture (-t) */
  char scrcpy_session[40];
  int scrcpy_audio_fd;
  int scrcpy_control_fd;
  GSource *scrcpy_control_source;
  XsScrcpyControl *scrcpy_control;
  uint8_t control_buffer[256 * 1024];
  size_t control_fill;
  bool scrcpy_config_sent;      /* parameter sets of this encoder sent */
  XsClipboard *clipboard;

  /* Desktop session. */
  char *rd_session_path;
  XsCapture *capture;
  XsInput *input;
  XsLogicalRect region;       /* our monitor, logical coordinates */
  bool have_region;
  guint monitors_changed_id;

  /* Encoding: `lock` serialises encoder submission between the PipeWire
   * thread (new frames) and the main thread (refinement). */
  pthread_mutex_t lock;
  XsEnc *enc;
  XsGpu *gpu;                 /* PipeWire thread only */
  XsDamageTracker tracker;
  bool capturing;             /* frames are wanted; false while stopping */
  int width;
  int height;
  int bitrate;
  atomic_bool keyframe_requested;
  atomic_bool reconfigure_pending;
  int64_t last_frame_us;
  bool refined;
  int64_t refine_pts;
  int64_t damage_since_refine;  /* pixels changed since the last keyframe */

  pthread_t output_thread;
  atomic_bool output_running;

  /* Stats, and their values at the previous report. */
  uint64_t frames_in;
  uint64_t frames_dropped;
  uint64_t frames_out;
  uint64_t bytes_out;
  uint64_t inputs;            /* input events from the client */
  /* Click to picture: when the last press arrived, and the delays from
   * presses to the first frame drawn after them (ms, this report). */
  _Atomic int64_t press_us;
  int64_t press_delay_max;
  int64_t press_delay_sum;
  int press_delays;
  int64_t converted_area;
  uint64_t reported_in;
  uint64_t reported_bytes;
  int64_t reported_area;
} Server;

static Server server;

static void start_extras (const char *display_name);
static void stop_extras (void);

/* Debug: XS_DEBUG_TIMING=1 reports, every 100 frames, how long after its
 * capture time each frame reached the stages of the pipeline. */
#define TIMING_SLOTS 64
#define TIMING_WINDOW 100

typedef struct
{
  int64_t pts;
  int64_t delivered, queued, encoded, sent;
} FrameTiming;

static struct
{
  bool enabled;
  pthread_mutex_t lock;
  FrameTiming slots[TIMING_SLOTS];
  FrameTiming window[TIMING_WINDOW];
  int n_window;
} timing = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int64_t now_us (void);

static FrameTiming *
timing_slot (int64_t pts)
{
  return &timing.slots[(uint64_t) pts / 1000 % TIMING_SLOTS];
}

static int
compare_int64 (const void *a, const void *b)
{
  int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
  return x < y ? -1 : x > y;
}

static void
timing_print_stage (const char *name, size_t offset)
{
  int64_t values[TIMING_WINDOW];
  int n = 0;

  for (int i = 0; i < timing.n_window; i++)
    {
      int64_t t = *(const int64_t *) ((const char *) &timing.window[i] + offset);
      if (t)
        values[n++] = t - timing.window[i].pts;
    }
  if (!n)
    return;
  qsort (values, n, sizeof values[0], compare_int64);
  fprintf (stderr, " %s p50 %.1f p90 %.1f max %.1f |", name,
           values[n / 2] / 1000.0, values[n * 9 / 10] / 1000.0, values[n - 1] / 1000.0);
}

static void
timing_mark (int64_t pts, size_t offset)
{
  FrameTiming *slot;

  if (!timing.enabled)
    return;

  pthread_mutex_lock (&timing.lock);
  slot = timing_slot (pts);
  if (slot->pts != pts)
    *slot = (FrameTiming) { .pts = pts };
  *(int64_t *) ((char *) slot + offset) = now_us ();

  if (offset == offsetof (FrameTiming, sent))
    {
      timing.window[timing.n_window++] = *slot;
      if (timing.n_window == TIMING_WINDOW)
        {
          fprintf (stderr, "xscreend: ms after capture:");
          timing_print_stage ("delivered", offsetof (FrameTiming, delivered));
          timing_print_stage ("queued", offsetof (FrameTiming, queued));
          timing_print_stage ("encoded", offsetof (FrameTiming, encoded));
          timing_print_stage ("sent", offsetof (FrameTiming, sent));
          fprintf (stderr, "\n");
          timing.n_window = 0;
        }
    }
  pthread_mutex_unlock (&timing.lock);
}

static void on_region (double x, double y, double width, double height, void *user_data);

static int64_t
now_us (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* --- client I/O -------------------------------------------------------- */

static bool
write_all (int fd, const void *data, size_t size)
{
  const uint8_t *p = data;

  while (size > 0)
    {
      ssize_t n = send (fd, p, size, MSG_NOSIGNAL);

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      p += n;
      size -= n;
    }
  return true;
}

/* Sends one protocol message (or, in raw mode, just `body`). */
static void
send_message (uint16_t type, const void *head, size_t head_size,
              const void *body, size_t body_size)
{
  XsMessageHeader header = {
    .magic = XS_PROTOCOL_MAGIC,
    .type = type,
    .length = head_size + body_size,
  };
  bool ok = true;

  pthread_mutex_lock (&server.write_lock);
  if (server.client_fd >= 0)
    {
      if (!server.raw)
        ok = write_all (server.client_fd, &header, sizeof header) &&
             (!head_size || write_all (server.client_fd, head, head_size));
      if (ok && body_size)
        ok = write_all (server.client_fd, body, body_size);
      if (!ok)
        shutdown (server.client_fd, SHUT_RDWR);
    }
  pthread_mutex_unlock (&server.write_lock);
}

/* Writes one scrcpy video header and its payload. */
static void
send_scrcpy_video (const uint8_t header[XS_SCRCPY_HEADER_SIZE], const void *body, size_t size)
{
  pthread_mutex_lock (&server.write_lock);
  if (server.client_fd >= 0 &&
      !(write_all (server.client_fd, header, XS_SCRCPY_HEADER_SIZE) &&
        (!size || write_all (server.client_fd, body, size))))
    shutdown (server.client_fd, SHUT_RDWR);
  pthread_mutex_unlock (&server.write_lock);
}

/* The client takes the parameter sets once, as a config packet it merges
 * into the next frame; repeats in later keyframes are dropped. */
static void
send_scrcpy_frame (const uint8_t *data, size_t size, int64_t pts_us, bool keyframe)
{
  uint8_t header[XS_SCRCPY_HEADER_SIZE];
  size_t sets = xs_scrcpy_parameter_sets_length (data, size);

  if (sets && !server.scrcpy_config_sent)
    {
      xs_scrcpy_packet_header (header, 0, true, false, sets);
      send_scrcpy_video (header, data, sets);
      server.scrcpy_config_sent = true;
    }
  if (sets == size)
    return;
  xs_scrcpy_packet_header (header, pts_us, false, keyframe, size - sets);
  send_scrcpy_video (header, data + sets, size - sets);
}

/* --- encoding ---------------------------------------------------------- */

static void
on_encoded (const uint8_t *data, size_t size, int64_t pts_us, bool keyframe, void *user_data)
{
  XsVideoFrame frame = {
    .pts_us = pts_us,
    .flags = (keyframe ? XS_FRAME_KEYFRAME : 0) |
             (pts_us == server.refine_pts ? XS_FRAME_REFINE : 0),
  };

  timing_mark (pts_us, offsetof (FrameTiming, encoded));
  if (server.scrcpy)
    send_scrcpy_frame (data, size, pts_us, keyframe);
  else if (server.raw)
    send_message (0, NULL, 0, data, size);
  else
    send_message (XS_MSG_VIDEO_FRAME, &frame, sizeof frame, data, size);
  timing_mark (pts_us, offsetof (FrameTiming, sent));
  if (server.frames_out == 0)
    fprintf (stderr, "xscreend: first frame %zu KiB\n", size / 1024);
  server.frames_out++;
  server.bytes_out += size;
}

static void *
output_thread (void *data)
{
  while (atomic_load (&server.output_running))
    if (xs_enc_poll_output (server.enc, 50, on_encoded, NULL) < 0)
      {
        fprintf (stderr, "xscreend: encoder output failed\n");
        break;
      }
  return NULL;
}

static void
stop_encoder (void)
{
  if (atomic_exchange (&server.output_running, false))
    pthread_join (server.output_thread, NULL);
  g_clear_pointer (&server.enc, xs_enc_free);
}

/* Called with `lock` held. */
static bool
start_encoder (int width, int height)
{
  XsEncConfig config = {
    .device = ENCODER_DEVICE,
    .codec = XS_CODEC_HEVC,
    .width = width,
    .height = height,
    .fps = MAX_FPS,
    .bitrate = server.bitrate,
    .n_inputs = 4,
    .i_frame_max_qp = I_FRAME_MAX_QP,
    .p_frame_max_qp = P_FRAME_MAX_QP,
  };
  XsVideoConfig video_config = { .codec = 1, .width = width, .height = height };
  char error[256];

  stop_encoder ();
  server.enc = xs_enc_new (&config, error, sizeof error);
  if (!server.enc)
    {
      fprintf (stderr, "xscreend: encoder: %s\n", error);
      return false;
    }
  server.width = width;
  server.height = height;
  xs_damage_tracker_reset (&server.tracker, width, height);
  atomic_store (&server.keyframe_requested, true);
  server.refined = true;
  server.damage_since_refine = 0;

  /* Announce the stream before its first frame can go out. */
  if (server.scrcpy)
    {
      uint8_t header[XS_SCRCPY_HEADER_SIZE];

      server.scrcpy_config_sent = false;
      xs_scrcpy_session_header (header, width, height);
      send_scrcpy_video (header, NULL, 0);
      if (server.scrcpy_control)
        xs_scrcpy_control_set_size (server.scrcpy_control, width, height);
    }
  else if (!server.raw)
    send_message (XS_MSG_VIDEO_CONFIG, &video_config, sizeof video_config, NULL, 0);

  atomic_store (&server.output_running, true);
  pthread_create (&server.output_thread, NULL, output_thread, NULL);
  fprintf (stderr, "xscreend: encoding %dx%d HEVC at %d Mbit/s\n",
           width, height, server.bitrate / 1000000);
  return true;
}

static void
wait_fence (int fence)
{
  if (fence >= 0)
    {
      struct pollfd pfd = { .fd = fence, .events = POLLIN };

      poll (&pfd, 1, 1000);
      close (fence);
    }
}

/* Debug (XS_DEBUG_RACE=1): checks whether a delivered frame is still being
 * written by the compositor's GPU, by hashing it on arrival and again 10 ms
 * later. */
static uint64_t
hash_source (const XsGpuSource *source, const XsRegion *damage)
{
  size_t size = (size_t) source->offset + (size_t) source->stride * source->height;
  struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
  const uint8_t *data = mmap (NULL, size, PROT_READ, MAP_SHARED, source->fd, 0);
  XsRect all = { 0, 0, source->width, source->height };
  uint64_t h = 1469598103934665603ull;

  if (data == MAP_FAILED)
    return 0;
  ioctl (source->fd, DMA_BUF_IOCTL_SYNC, &sync);
  for (int r = 0; r < (damage->full ? 1 : damage->n_rects); r++)
    {
      const XsRect *rect = damage->full ? &all : &damage->rects[r];

      for (int y = rect->y; y < rect->y + rect->height; y += 7)
        for (int x = rect->x; x < rect->x + rect->width; x += 13)
          {
            h ^= *(const uint32_t *) (data + source->offset + (size_t) y * source->stride + x * 4);
            h *= 1099511628211ull;
          }
    }
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ioctl (source->fd, DMA_BUF_IOCTL_SYNC, &sync);
  munmap ((void *) data, size);
  return h;
}

/* PipeWire thread. */
static void
on_frame (const XsGpuSource *source, const XsRegion *damage, int64_t pts_us,
          void *user_data)
{
  const XsEncInput *input;
  XsGpuTarget target;
  XsRegion repair;
  char error[256];
  int slot;

  timing_mark (pts_us, offsetof (FrameTiming, delivered));
  pthread_mutex_lock (&server.lock);

  /* A frame still in flight while the session stops must not bring the
   * GPU back: its context would stay current on a thread that is about
   * to exit, and the next session's conversions would silently do
   * nothing. */
  if (!server.capturing)
    goto out;

  if (!server.gpu)
    {
      server.gpu = xs_gpu_new (error, sizeof error);
      if (!server.gpu)
        {
          fprintf (stderr, "xscreend: gpu: %s\n", error);
          goto out;
        }
    }

  if (atomic_exchange (&server.reconfigure_pending, false) ||
      !server.enc || source->width != server.width || source->height != server.height)
    {
      xs_gpu_forget_sources (server.gpu);
      xs_gpu_forget_targets (server.gpu);
      if (!start_encoder (source->width, source->height))
        goto out;
    }

  {
    int64_t press = atomic_load (&server.press_us);

    if (press && pts_us > press)
      {
        int64_t delay = (pts_us - press) / 1000;

        server.press_delay_sum += delay;
        server.press_delay_max = MAX (server.press_delay_max, delay);
        server.press_delays++;
        atomic_store (&server.press_us, 0);
      }
  }
  server.frames_in++;
  server.damage_since_refine += xs_region_area (damage, source->width, source->height);
  if (getenv ("XS_DEBUG_RACE"))
    {
      static unsigned checked, raced;
      uint64_t before = hash_source (source, damage);

      usleep (10000);
      checked++;
      raced += hash_source (source, damage) != before;
      if (checked % 20 == 0)
        fprintf (stderr, "xscreend: race check: %u of %u frames changed after delivery\n",
                 raced, checked);
    }
  xs_damage_tracker_add_frame (&server.tracker, damage);

  /* No free input: the encoder is behind. Drop this frame; its damage
   * stays in the history and the next frame repairs it. */
  slot = xs_enc_acquire_input (server.enc, 0);
  if (slot < 0)
    {
      server.frames_dropped++;
      goto out;
    }

  input = xs_enc_get_input (server.enc, slot);
  target = (XsGpuTarget) {
    .fd = input->dmabuf_fd,
    .width = server.width,
    .height = server.height,
    .stride = input->stride,
    .uv_offset = input->uv_offset,
  };
  xs_damage_tracker_get_repair (&server.tracker, slot, &repair);
  server.converted_area += xs_region_area (&repair, server.width, server.height);

  wait_fence (xs_gpu_convert (server.gpu, source, &target, &repair));
  xs_damage_tracker_mark_slot (&server.tracker, slot);

  xs_enc_queue_input (server.enc, slot, pts_us,
                      atomic_exchange (&server.keyframe_requested, false));
  timing_mark (pts_us, offsetof (FrameTiming, queued));
  server.last_frame_us = now_us ();
  server.refined = false;

out:
  pthread_mutex_unlock (&server.lock);
}

/* Debug: XS_DUMP_REFINE=path saves the NV12 picture behind a refinement
 * keyframe, so its quality can be measured against the decoded stream. */
static void
maybe_dump_input (int slot)
{
  const char *path = getenv ("XS_DUMP_REFINE");
  const XsEncInput *input = xs_enc_get_input (server.enc, slot);
  FILE *f;
  void *data;

  if (!path || !input)
    return;
  data = mmap (NULL, input->size, PROT_READ, MAP_SHARED, input->dmabuf_fd, 0);
  if (data == MAP_FAILED)
    return;
  f = fopen (path, "wb");
  if (f)
    {
      /* Tightly packed NV12, as ffmpeg's rawvideo expects. */
      for (int y = 0; y < server.height; y++)
        fwrite ((uint8_t *) data + (size_t) y * input->stride, 1, server.width, f);
      for (int y = 0; y < server.height / 2; y++)
        fwrite ((uint8_t *) data + input->uv_offset + (size_t) y * input->stride, 1,
                server.width, f);
      fclose (f);
    }
  munmap (data, input->size);
}

/* Main thread: once the picture has been still for a moment, encode it
 * once more as a keyframe, whose tight QP cap makes text and fine detail
 * sharp. Nothing is sent while it stays still. */
static gboolean
on_refine_timer (gpointer user_data)
{
  pthread_mutex_lock (&server.lock);
  if (server.enc && !server.refined &&
      now_us () - server.last_frame_us > REFINE_DELAY_US &&
      server.damage_since_refine <
        REFINE_MIN_DAMAGE * server.width * server.height)
    server.refined = true;   /* not worth a keyframe */

  if (server.enc && !server.refined &&
      now_us () - server.last_frame_us > REFINE_DELAY_US)
    {
      /* Only the slot holding the newest frame can be re-encoded without
       * the GPU (which lives on the PipeWire thread); wait until the
       * hardware has released it. */
      for (int slot = 0; slot < xs_enc_get_n_inputs (server.enc); slot++)
        {
          XsRegion repair;

          xs_damage_tracker_get_repair (&server.tracker, slot, &repair);
          if (!xs_region_is_empty (&repair) || !xs_enc_input_is_free (server.enc, slot))
            continue;

          maybe_dump_input (slot);
          server.refine_pts = now_us ();
          xs_enc_queue_input (server.enc, slot, server.refine_pts, true);
          server.refined = true;
          server.damage_since_refine = 0;
          break;
        }
    }
  pthread_mutex_unlock (&server.lock);
  return G_SOURCE_CONTINUE;
}

static void
reset_stats (void)
{
  server.frames_in = server.frames_out = server.frames_dropped = 0;
  server.bytes_out = 0;
  server.inputs = 0;
  server.converted_area = 0;
  server.reported_in = server.reported_bytes = 0;
  server.reported_area = 0;
}

static gboolean
on_stats_timer (gpointer user_data)
{
  uint64_t in = server.frames_in - server.reported_in;

  if (server.client_fd < 0)
    return G_SOURCE_CONTINUE;
  fprintf (stderr,
           "xscreend: %llu frames in, %llu out, %llu dropped (5 s: %llu in, %.1f Mbit/s, "
           "converted %.1f%% of full frames, %llu input events, "
           "press to frame avg %lld max %lld ms over %d)\n",
           (unsigned long long) server.frames_in, (unsigned long long) server.frames_out,
           (unsigned long long) server.frames_dropped, (unsigned long long) in,
           (server.bytes_out - server.reported_bytes) * 8 / 5.0 / 1e6,
           in && server.width ?
             100.0 * (server.converted_area - server.reported_area) /
             ((double) in * server.width * server.height) : 0.0,
           (unsigned long long) server.inputs,
           server.press_delays ? (long long) (server.press_delay_sum / server.press_delays) : 0,
           (long long) server.press_delay_max, server.press_delays);
  server.inputs = 0;
  server.press_delay_sum = server.press_delay_max = 0;
  server.press_delays = 0;
  server.reported_in = server.frames_in;
  server.reported_bytes = server.bytes_out;
  server.reported_area = server.converted_area;
  return G_SOURCE_CONTINUE;
}

/* --- desktop session --------------------------------------------------- */

static void
on_cursor (const XsCursorInfo *info, void *user_data)
{
  XsCursor cursor = {
    .x = info->x,
    .y = info->y,
    .hotspot_x = info->hotspot_x,
    .hotspot_y = info->hotspot_y,
    .visible = info->visible,
    .bitmap_width = info->bitmap ? info->bitmap_width : 0,
    .bitmap_height = info->bitmap ? info->bitmap_height : 0,
  };

  if (server.raw || server.scrcpy)   /* scrcpy has no cursor channel */
    return;
  send_message (XS_MSG_CURSOR, &cursor, sizeof cursor, info->bitmap,
                info->bitmap ? (size_t) info->bitmap_width * info->bitmap_height * 4 : 0);
}

/* PipeWire thread: the GL context is current there. */
static void
on_buffers_changed (void *user_data)
{
  pthread_mutex_lock (&server.lock);
  if (server.gpu)
    xs_gpu_forget_sources (server.gpu);
  pthread_mutex_unlock (&server.lock);
}

static void
free_gpu_in_thread (void *data)
{
  pthread_mutex_lock (&server.lock);
  g_clear_pointer (&server.gpu, xs_gpu_free);
  pthread_mutex_unlock (&server.lock);
}

static void
stop_desktop_session (void)
{
  g_clear_pointer (&server.clipboard, xs_clipboard_free);
  g_clear_pointer (&server.input, xs_input_free);
  server.have_region = false;
  pthread_mutex_lock (&server.lock);
  server.capturing = false;
  pthread_mutex_unlock (&server.lock);
  if (server.capture)
    xs_capture_invoke (server.capture, free_gpu_in_thread, NULL);
  g_clear_pointer (&server.capture, xs_capture_free);

  pthread_mutex_lock (&server.lock);
  stop_encoder ();
  pthread_mutex_unlock (&server.lock);

  if (server.rd_session_path)
    {
      g_dbus_connection_call_sync (server.bus, RD, server.rd_session_path, RD ".Session",
                                   "Stop", NULL, NULL, 0, -1, NULL, NULL);
      g_clear_pointer (&server.rd_session_path, g_free);
    }
}

static bool
start_desktop_session (const XsMonitorSpec *spec, int width, int height, GError **error)
{
  XsCaptureCallbacks callbacks = {
    .frame = on_frame,
    .cursor = on_cursor,
    .buffers_changed = on_buffers_changed,
  };
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) id = NULL;

  /* RemoteDesktop first: the screen cast links to it so input can target
   * the virtual monitor's stream. */
  reply = g_dbus_connection_call_sync (server.bus, RD, "/org/gnome/Mutter/RemoteDesktop", RD,
                                       "CreateSession", NULL, G_VARIANT_TYPE ("(o)"),
                                       0, -1, NULL, error);
  if (!reply)
    return false;
  g_variant_get (reply, "(o)", &server.rd_session_path);
  g_clear_pointer (&reply, g_variant_unref);

  reply = g_dbus_connection_call_sync (server.bus, RD, server.rd_session_path,
                                       "org.freedesktop.DBus.Properties", "Get",
                                       g_variant_new ("(ss)", RD ".Session", "SessionId"),
                                       G_VARIANT_TYPE ("(v)"), 0, -1, NULL, error);
  if (!reply)
    return false;
  g_variant_get (reply, "(v)", &id);

  server.capture = xs_capture_new (server.bus, g_variant_get_string (id, NULL),
                                   spec, width, height,
                                   spec ? (int) spec->modes[0].refresh_rate : MAX_FPS,
                                   server.scrcpy_cursor, &callbacks, error);
  if (!server.capture)
    return false;
  pthread_mutex_lock (&server.lock);
  server.capturing = true;
  pthread_mutex_unlock (&server.lock);

  if (!g_dbus_connection_call_sync (server.bus, RD, server.rd_session_path, RD ".Session",
                                    "Start", NULL, NULL, 0, -1, NULL, error))
    return false;

  if (!xs_capture_start (server.capture, false, error))
    return false;

  server.input = xs_input_new (server.bus, server.rd_session_path,
                               xs_capture_get_stream_path (server.capture),
                               on_region, NULL, error);
  return server.input != NULL;
}

/* Tells the client which edges lead to another monitor. */
static void
send_layout (void)
{
  XsLayout layout;

  if (!server.have_region || server.raw || server.scrcpy)
    return;
  layout.edges = xs_layout_get_edges (server.bus, &server.region);
  fprintf (stderr, "xscreend: crossing edges:%s%s%s%s\n",
           layout.edges & XS_EDGE_LEFT ? " left" : "",
           layout.edges & XS_EDGE_RIGHT ? " right" : "",
           layout.edges & XS_EDGE_TOP ? " top" : "",
           layout.edges & XS_EDGE_BOTTOM ? " bottom" : "");
  send_message (XS_MSG_LAYOUT, &layout, sizeof layout, NULL, 0);
}

static void
on_region (double x, double y, double width, double height, void *user_data)
{
  server.region = (XsLogicalRect) { x, y, width, height };
  server.have_region = true;
  send_layout ();
}

static void
on_monitors_changed (GDBusConnection *connection, const char *sender, const char *path,
                     const char *iface, const char *signal, GVariant *params,
                     gpointer user_data)
{
  send_layout ();
}

/* --- client protocol --------------------------------------------------- */

static void
disconnect_client (void)
{
  if (server.client_source)
    {
      g_source_destroy (server.client_source);
      g_clear_pointer (&server.client_source, g_source_unref);
    }
  stop_extras ();
  stop_desktop_session ();

  if (server.scrcpy_control_source)
    {
      g_source_destroy (server.scrcpy_control_source);
      g_clear_pointer (&server.scrcpy_control_source, g_source_unref);
    }
  pthread_mutex_lock (&server.write_lock);
  if (server.client_fd >= 0)
    close (server.client_fd);
  server.client_fd = -1;
  if (server.scrcpy_control_fd >= 0)
    close (server.scrcpy_control_fd);
  server.scrcpy_control_fd = -1;
  if (server.scrcpy_audio_fd >= 0)
    close (server.scrcpy_audio_fd);
  server.scrcpy_audio_fd = -1;
  pthread_mutex_unlock (&server.write_lock);
  g_clear_pointer (&server.scrcpy_control, xs_scrcpy_control_free);
  server.scrcpy = false;
  server.scrcpy_cursor = false;
  server.scrcpy_session[0] = '\0';
  server.control_fill = 0;
  server.read_fill = 0;
  if (server.read_capacity > 1024 * 1024)
    {
      /* A clipboard image grew it; give the memory back. */
      g_clear_pointer (&server.read_buffer, g_free);
      server.read_capacity = 0;
    }
  fprintf (stderr, "xscreend: client disconnected (%llu frames, %.1f MiB sent)\n",
           (unsigned long long) server.frames_out, server.bytes_out / 1048576.0);
}

static int
align_down (int value, int alignment)
{
  return value / alignment * alignment;
}

static bool
add_mode (XsMonitorSpec *spec, int width, int height, double refresh, double scale,
          bool preferred)
{
  XsMonitorMode *m;

  width = align_down (width, 8);
  height = align_down (height, 2);
  if (width < 640 || height < 480 || spec->n_modes == XS_MONITOR_MAX_MODES)
    return false;
  for (int i = 0; i < spec->n_modes; i++)
    if (spec->modes[i].width == width && spec->modes[i].height == height &&
        spec->modes[i].refresh_rate == refresh)
      return false;

  m = &spec->modes[spec->n_modes++];
  *m = (XsMonitorMode) { width, height, refresh, scale, preferred };
  return true;
}

/* Common monitor resolutions in several shapes, like a real display's
 * EDID: 16:9, 16:10, 3:2, 4:3 and 21:9. */
static const struct { int width, height; } standard_modes[] = {
  { 3840, 2400 }, { 3840, 2160 }, { 3440, 1440 }, { 3200, 1800 }, { 3000, 2000 },
  { 2880, 1800 }, { 2560, 1600 }, { 2560, 1440 }, { 2560, 1080 }, { 2304, 1440 },
  { 2160, 1440 }, { 2048, 1536 }, { 2048, 1152 }, { 1920, 1280 }, { 1920, 1200 },
  { 1920, 1080 }, { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 }, { 1440, 960 },
  { 1440, 900 }, { 1400, 1050 }, { 1368, 768 }, { 1280, 960 }, { 1280, 800 },
  { 1280, 720 }, { 1024, 768 },
};

/* Largest picture offered as the preferred mode: 4K-class, so a 5K or 6K
 * display costs the phone no more than a 4K one (larger stays selectable). */
#define PREFERRED_MAX_PIXELS (3840 * 2160)

/* Scale GNOME suggests for a mode: the client's HiDPI factor when the
 * picture stays roomy at that scale, 1x otherwise. */
static double
suggested_scale (int width, double hidpi)
{
  return hidpi > 1.0 && width / hidpi >= 1280 ? hidpi : 1.0;
}

/* Adds a resolution at the display's refresh rate, plus 60 Hz when the
 * display is faster. */
static void
add_resolution (XsMonitorSpec *spec, int width, int height, double refresh, double hidpi,
                bool preferred)
{
  double scale = suggested_scale (width, hidpi);

  add_mode (spec, width, height, refresh, scale, preferred);
  if (refresh > 61)
    add_mode (spec, width, height, 60.0, scale, false);
}

/* Modes offered to GNOME, which chooses among them (GNOME Settings): the
 * client's window shown 1:1 (preferred), its whole display (full screen),
 * smaller sizes of the display's own shape, and common resolutions of
 * other shapes that fit on the display. */
static void
build_monitor_spec (const XsClientHello *hello, XsMonitorSpec *spec,
                    char *serial, size_t serial_size, char *product, size_t product_size)
{
  static const double native_fractions[] = { 3.0 / 4.0, 2.0 / 3.0, 1.0 / 2.0 };
  double hidpi = CLAMP (hello->scale_percent, 100, 400) / 100.0;
  double refresh = CLAMP ((int) hello->max_fps, 30, 240);
  int native_w = CLAMP ((int) hello->width, 640, 7680);
  int native_h = CLAMP ((int) hello->height, 480, 4320);

  g_strlcpy (serial, hello->display_id[0] ? hello->display_id : "default",
             MIN (serial_size, sizeof hello->display_id));
  g_strlcpy (product, hello->display_name[0] ? hello->display_name : "Display",
             MIN (product_size, sizeof hello->display_name));

  spec->vendor = "XScreen";
  spec->product = product;
  spec->serial = serial;
  spec->n_modes = 0;

  /* The preferred mode must come first: mutter takes the first one. A
   * window size means the client shows a window; otherwise the whole
   * display, at most PREFERRED_MAX_PIXELS, with the scale that keeps
   * GNOME's interface the size of the client's own. */
  if (hello->window_width && hello->window_height)
    add_mode (spec, hello->window_width, hello->window_height, refresh,
              hidpi, true);
  else if ((double) native_w * native_h > PREFERRED_MAX_PIXELS)
    {
      double fit = sqrt (PREFERRED_MAX_PIXELS / ((double) native_w * native_h));
      double scale = MAX (1.0, round (hidpi * fit * 4) / 4);

      add_mode (spec, native_w * fit, native_h * fit, refresh, scale, true);
    }
  add_resolution (spec, native_w, native_h, refresh, hidpi, spec->n_modes == 0);
  for (size_t i = 0; i < G_N_ELEMENTS (native_fractions); i++)
    add_resolution (spec, native_w * native_fractions[i], native_h * native_fractions[i],
                    refresh, hidpi, false);
  for (size_t i = 0; i < G_N_ELEMENTS (standard_modes); i++)
    if (standard_modes[i].width <= native_w && standard_modes[i].height <= native_h)
      add_resolution (spec, standard_modes[i].width, standard_modes[i].height,
                      refresh, hidpi, false);
  if (refresh > 61 && hello->window_width && hello->window_height)
    add_mode (spec, hello->window_width, hello->window_height, 60.0, hidpi, false);
}

static bool
handle_message (const XsMessageHeader *header, const uint8_t *payload)
{
  g_autoptr (GError) error = NULL;

  switch (header->type)
    {
    case XS_MSG_CLIENT_HELLO:
      {
        const XsClientHello *hello = (const XsClientHello *) payload;
        char serial[64], product[64];
        XsMonitorSpec spec;

        if (header->length < sizeof *hello || server.capture)
          return false;
        build_monitor_spec (hello, &spec, serial, sizeof serial, product, sizeof product);
        fprintf (stderr, "xscreend: client display '%s' (%s): %dx%d@%d, %u%%\n",
                 product, serial, spec.modes[0].width, spec.modes[0].height,
                 (int) spec.modes[0].refresh_rate, hello->scale_percent);
        if (!start_desktop_session (&spec, spec.modes[0].width, spec.modes[0].height, &error))
          {
            fprintf (stderr, "xscreend: desktop session: %s\n", error->message);
            return false;
          }
        start_extras (product);
        return true;
      }
    case XS_MSG_REQUEST_KEYFRAME:
      atomic_store (&server.keyframe_requested, true);
      return true;
    case XS_MSG_INPUT:
      if (server.input)
        for (size_t off = 0; off + sizeof (XsInputEvent) <= header->length;
             off += sizeof (XsInputEvent))
          {
            XsInputEvent event;

            memcpy (&event, payload + off, sizeof event);
            xs_input_handle (server.input, &event);
            server.inputs++;
            if ((event.type == XS_INPUT_BUTTON || event.type == XS_INPUT_KEY) && event.value == 1)
              {
                int64_t none = 0;
                atomic_compare_exchange_strong (&server.press_us, &none, now_us ());
              }
          }
      return true;
    case XS_MSG_CLIENT_CLIPBOARD:
      {
        XsClipboardHeader head;
        g_autoptr (GBytes) content = NULL;

        if (header->length < sizeof head)
          return false;
        memcpy (&head, payload, sizeof head);
        content = g_bytes_new (payload + sizeof head, header->length - sizeof head);
        if (server.clipboard && (head.format == XS_CLIPBOARD_TEXT ||
                                 head.format == XS_CLIPBOARD_IMAGE))
          xs_clipboard_set (server.clipboard,
                            head.format == XS_CLIPBOARD_IMAGE ? XS_CLIP_IMAGE : XS_CLIP_TEXT,
                            content);
        return true;
      }
    case XS_MSG_CLIENT_FILE_BEGIN:
    case XS_MSG_CLIENT_FILE_DATA:
    case XS_MSG_CLIENT_FILE_END:
      return !server.file_receiver ||
             xs_file_receiver_handle (server.file_receiver, header->type, payload,
                                      header->length);
    default:
      return true;
    }
}

static gboolean
on_client_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  ssize_t n;
  size_t offset = 0;

  if (server.read_capacity - server.read_fill < 64 * 1024)
    {
      server.read_capacity = MAX (server.read_capacity * 2, 256 * 1024);
      server.read_buffer = g_realloc (server.read_buffer, server.read_capacity);
    }
  n = recv (fd, server.read_buffer + server.read_fill, server.read_capacity - server.read_fill, 0);

  if (n <= 0)
    {
      disconnect_client ();
      return G_SOURCE_REMOVE;
    }
  if (server.scrcpy)
    {
      /* The scrcpy client never writes on its video stream. */
      disconnect_client ();
      return G_SOURCE_REMOVE;
    }
  if (server.raw)
    return G_SOURCE_CONTINUE;

  server.read_fill += n;
  while (server.read_fill - offset >= sizeof (XsMessageHeader))
    {
      XsMessageHeader header;

      memcpy (&header, server.read_buffer + offset, sizeof header);
      if (header.magic != XS_PROTOCOL_MAGIC ||
          header.length > XS_CLIPBOARD_MAX + XS_FILE_CHUNK)
        {
          disconnect_client ();
          return G_SOURCE_REMOVE;
        }
      if (server.read_fill - offset < sizeof header + header.length)
        {
          /* Room for the whole message once the bytes before it go. */
          size_t need = sizeof header + header.length + 64 * 1024;

          if (need > server.read_capacity)
            {
              memmove (server.read_buffer, server.read_buffer + offset,
                       server.read_fill - offset);
              server.read_fill -= offset;
              server.read_capacity = need;
              server.read_buffer = g_realloc (server.read_buffer, need);
              return G_SOURCE_CONTINUE;
            }
          break;
        }
      if (!handle_message (&header, server.read_buffer + offset + sizeof header))
        {
          disconnect_client ();
          return G_SOURCE_REMOVE;
        }
      offset += sizeof header + header.length;
    }
  memmove (server.read_buffer, server.read_buffer + offset, server.read_fill - offset);
  server.read_fill -= offset;
  return G_SOURCE_CONTINUE;
}

/* Only root and adb (the shell user, which runs adb forwards and the
 * scrcpy relay) may drive the desktop; any app could reach a loopback port. */
static bool
peer_allowed (int fd)
{
  static const char *const tables[] = { "/proc/net/tcp", "/proc/net/tcp6" };
  struct sockaddr_storage local, peer;
  socklen_t local_len = sizeof local, peer_len = sizeof peer;
  unsigned local_port, peer_port;

  if (getsockname (fd, (struct sockaddr *) &local, &local_len) < 0 ||
      getpeername (fd, (struct sockaddr *) &peer, &peer_len) < 0)
    return false;
  local_port = ntohs (((struct sockaddr_in *) &local)->sin_port);
  peer_port = ntohs (((struct sockaddr_in *) &peer)->sin_port);

  /* The peer's own socket: its local port is our peer port, and its
   * remote port is our local one. */
  for (size_t t = 0; t < G_N_ELEMENTS (tables); t++)
    {
      g_autofree char *contents = NULL;
      g_auto (GStrv) lines = NULL;

      if (!g_file_get_contents (tables[t], &contents, NULL, NULL))
        continue;
      lines = g_strsplit (contents, "\n", -1);
      for (char **l = lines + 1; *l; l++)
        {
          unsigned lport, rport, uid;

          if (sscanf (*l, "%*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%x %*x %*x:%*x %*x:%*x %*x %u",
                      &lport, &rport, &uid) == 3 &&
              lport == peer_port && rport == local_port)
            {
              if (uid == 0 || uid == 2000)
                return true;
              fprintf (stderr, "xscreend: refused a connection from uid %u\n", uid);
              return false;
            }
        }
    }
  fprintf (stderr, "xscreend: refused a connection from an unknown peer\n");
  return false;
}

static gboolean
on_listen_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  int client = accept4 (fd, NULL, NULL, SOCK_CLOEXEC);
  int one = 1;
  XsServerHello hello = { .version = XS_PROTOCOL_VERSION, .codecs = 1 };
  g_autoptr (GError) error = NULL;

  if (client < 0)
    return G_SOURCE_CONTINUE;
  if (server.client_fd >= 0 || !peer_allowed (client))
    {
      close (client);
      return G_SOURCE_CONTINUE;
    }
  setsockopt (client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  pthread_mutex_lock (&server.write_lock);
  server.client_fd = client;
  pthread_mutex_unlock (&server.write_lock);
  reset_stats ();
  fprintf (stderr, "xscreend: client connected\n");

  server.client_source = g_unix_fd_source_new (client, G_IO_IN | G_IO_HUP | G_IO_ERR);
  g_source_set_callback (server.client_source, G_SOURCE_FUNC (on_client_readable), NULL, NULL);
  g_source_attach (server.client_source, NULL);

  if (server.raw)
    {
      if (!start_desktop_session (NULL, server.raw_width, server.raw_height, &error))
        {
          fprintf (stderr, "xscreend: desktop session: %s\n", error->message);
          disconnect_client ();
        }
    }
  else
    send_message (XS_MSG_SERVER_HELLO, &hello, sizeof hello, NULL, 0);
  return G_SOURCE_CONTINUE;
}

/* --- scrcpy client ----------------------------------------------------- */

static void
send_scrcpy_control (const void *data, size_t size)
{
  pthread_mutex_lock (&server.write_lock);
  if (server.scrcpy_control_fd >= 0 && !write_all (server.scrcpy_control_fd, data, size))
    shutdown (server.scrcpy_control_fd, SHUT_RDWR);
  pthread_mutex_unlock (&server.write_lock);
}

static void
send_scrcpy_clipboard (const char *text)
{
  size_t size = strlen (text) + 5;
  g_autofree uint8_t *message = g_malloc (size);

  size = xs_scrcpy_clipboard_message (message, size, text);
  send_scrcpy_control (message, size);
}

static void
on_clipboard_changed (XsClipKind kind, GBytes *content, void *user_data)
{
  g_autofree char *text = NULL;

  if (server.scrcpy)
    {
      if (kind == XS_CLIP_TEXT)
        {
          text = g_strndup (g_bytes_get_data (content, NULL), g_bytes_get_size (content));
          send_scrcpy_clipboard (text);
        }
      return;
    }
  if (kind == XS_CLIP_FILES)
    {
      g_autofree char *list = g_strndup (g_bytes_get_data (content, NULL),
                                         g_bytes_get_size (content));
      g_auto (GStrv) uris = g_uri_list_extract_uris (list);
      g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GError) error = NULL;

      for (int i = 0; uris[i]; i++)
        {
          char *path = g_filename_from_uri (uris[i], NULL, NULL);
          if (path)
            g_ptr_array_add (paths, path);
        }
      g_ptr_array_add (paths, NULL);
      if (server.file_sender &&
          !xs_file_sender_send (server.file_sender, (const char *const *) paths->pdata,
                                XS_FILE_FOR_CLIPBOARD, XS_FILE_CLIPBOARD_MAX, &error))
        fprintf (stderr, "xscreend: copied files not shared: %s\n", error->message);
      return;
    }
  {
    XsClipboardHeader head = { .format = kind == XS_CLIP_IMAGE ? XS_CLIPBOARD_IMAGE : XS_CLIPBOARD_TEXT };

    send_message (XS_MSG_CLIPBOARD, &head, sizeof head, g_bytes_get_data (content, NULL),
                  g_bytes_get_size (content));
  }
}

/* --- xscreen client extras: sound, files ------------------------------- */

static void
on_audio (const int16_t *pcm, size_t frames, int64_t pts_us, void *user_data)
{
  XsAudioFrame frame = {
    .pts_us = pts_us,
    .rate = XS_AUDIO_RATE,
    .channels = XS_AUDIO_CHANNELS,
  };

  send_message (XS_MSG_AUDIO, &frame, sizeof frame, pcm,
                frames * XS_AUDIO_CHANNELS * sizeof (int16_t));
}

static void
send_file_message (uint16_t type, const void *head, size_t head_size,
                   const void *body, size_t body_size, void *user_data)
{
  send_message (type, head, head_size, body, body_size);
}

static void
on_notification_action (GDBusConnection *connection, const char *sender, const char *path,
                        const char *iface, const char *signal, GVariant *params,
                        gpointer user_data)
{
  guint32 id;
  const char *action;
  const char *downloads = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
  g_autofree char *folder = downloads ? g_strdup (downloads) :
                            g_build_filename (g_get_home_dir (), "Downloads", NULL);
  g_autofree char *uri = g_filename_to_uri (folder, NULL, NULL);

  g_variant_get (params, "(u&s)", &id, &action);
  if (id && id == server.download_notification && uri)
    g_app_info_launch_default_for_uri (uri, NULL, NULL);
}

static void
on_download_notified (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                              result, NULL);
  if (reply)
    g_variant_get (reply, "(u)", &server.download_notification);
}

static void
on_files_received (uint32_t purpose, const char *const *paths, void *user_data)
{
  if (purpose == XS_FILE_FOR_CLIPBOARD)
    {
      GString *list = g_string_new (NULL);
      g_autoptr (GBytes) content = NULL;

      for (int i = 0; paths[i]; i++)
        {
          g_autofree char *uri = g_filename_to_uri (paths[i], NULL, NULL);
          if (uri)
            g_string_append_printf (list, "%s\r\n", uri);
        }
      content = g_string_free_to_bytes (list);
      if (server.clipboard)
        xs_clipboard_set (server.clipboard, XS_CLIP_FILES, content);
      return;
    }
  {
    guint n = g_strv_length ((char **) paths);
    g_autofree char *first = g_path_get_basename (paths[0]);
    g_autofree char *summary = n == 1 ? g_strdup ("已从 Mac 收到文件") :
                               g_strdup_printf ("已从 Mac 收到 %u 个项目", n);
    g_autofree char *body = n == 1 ? g_strdup (first) :
                            g_strdup_printf ("%s 等，已保存到“下载”", first);
    const char *actions[] = { "default", "打开“下载”", NULL };

    g_dbus_connection_call (server.bus, "org.freedesktop.Notifications",
                            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                            "Notify",
                            g_variant_new ("(susss^asa{sv}i)", "XScreen", 0, "folder-download",
                                           summary, body, actions, NULL, -1),
                            G_VARIANT_TYPE ("(u)"), 0, -1, NULL, on_download_notified, NULL);
  }
}

static void
start_extras (const char *display_name)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *description = g_strdup_printf ("XScreen（%s）", display_name);

  server.clipboard = xs_clipboard_new (server.bus, server.rd_session_path,
                                       on_clipboard_changed, NULL, &error);
  if (!server.clipboard)
    {
      fprintf (stderr, "xscreend: no clipboard sharing: %s\n", error->message);
      g_clear_error (&error);
    }
  server.audio = xs_audio_new (description, on_audio, NULL, &error);
  if (!server.audio)
    fprintf (stderr, "xscreend: no sound: %s\n", error->message);
  server.file_sender = xs_file_sender_new (send_file_message, NULL);
  server.file_receiver = xs_file_receiver_new (on_files_received, NULL);
}

static void
stop_extras (void)
{
  g_clear_pointer (&server.audio, xs_audio_free);
  g_clear_pointer (&server.file_sender, xs_file_sender_free);
  g_clear_pointer (&server.file_receiver, xs_file_receiver_free);
}

/* local.xscreen.XScreen.SendFiles: Nautilus's "Send to Mac" script. */
static const char daemon_xml[] =
  "<node><interface name='local.xscreen.XScreen'>"
  "<method name='SendFiles'><arg type='as' name='uris' direction='in'/></method>"
  "</interface></node>";

static void
on_daemon_call (GDBusConnection *connection, const char *sender, const char *path,
                const char *iface, const char *method, GVariant *params,
                GDBusMethodInvocation *invocation, gpointer user_data)
{
  g_autofree const char **uris = NULL;
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GError) error = NULL;

  if (!server.file_sender)
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR,
                                                     G_IO_ERROR_NOT_CONNECTED,
                                                     "XScreen 未连接");
      return;
    }
  g_variant_get (params, "(^a&s)", &uris);
  for (int i = 0; uris[i]; i++)
    {
      char *local = g_filename_from_uri (uris[i], NULL, NULL);
      if (local)
        g_ptr_array_add (paths, local);
    }
  g_ptr_array_add (paths, NULL);
  if (paths->len == 1 ||
      !xs_file_sender_send (server.file_sender, (const char *const *) paths->pdata,
                            XS_FILE_FOR_DOWNLOADS, 0, &error))
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
                                                     error ? error->message : "没有可发送的本地文件");
      return;
    }
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
export_daemon (void)
{
  static const GDBusInterfaceVTable vtable = { .method_call = on_daemon_call };
  g_autoptr (GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (daemon_xml, NULL);

  g_dbus_connection_register_object (server.bus, "/local/xscreen/XScreen", info->interfaces[0],
                                     &vtable, NULL, NULL, NULL);
  g_bus_own_name_on_connection (server.bus, "local.xscreen.XScreen",
                                G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
  g_dbus_connection_signal_subscribe (server.bus, "org.freedesktop.Notifications",
                                      "org.freedesktop.Notifications", "ActionInvoked",
                                      "/org/freedesktop/Notifications", NULL, 0,
                                      on_notification_action, NULL, NULL);
}

static void
scrcpy_input (const XsInputEvent *event, void *user_data)
{
  if (server.input)
    xs_input_handle (server.input, event);
  server.inputs++;
}

static void
scrcpy_keyframe (void *user_data)
{
  atomic_store (&server.keyframe_requested, true);
}

static void
press_ctrl (uint16_t key)
{
  static const int sequence[][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };

  for (size_t i = 0; i < G_N_ELEMENTS (sequence); i++)
    scrcpy_input (&(XsInputEvent) { .type = XS_INPUT_KEY,
                                    .code = sequence[i][0] ? key : KEY_LEFTCTRL,
                                    .value = sequence[i][1] }, NULL);
}

static void
scrcpy_get_clipboard (int copy_key, void *user_data)
{
  g_autofree char *text = NULL;

  if (!server.clipboard)
    return;
  /* After a copy or cut, the new clipboard owner's text is sent when GNOME
   * announces it (on_clipboard_changed). */
  if (copy_key == 1 || copy_key == 2)
    {
      press_ctrl (copy_key == 1 ? KEY_C : KEY_X);
      return;
    }
  text = xs_clipboard_get_text (server.clipboard);
  if (text)
    send_scrcpy_clipboard (text);
}

static void
scrcpy_set_clipboard (const char *text, bool paste, uint64_t sequence, void *user_data)
{
  uint8_t ack[9];

  if (server.clipboard)
    xs_clipboard_set_text (server.clipboard, text);
  if (paste)
    press_ctrl (KEY_V);
  if (sequence)
    send_scrcpy_control (ack, xs_scrcpy_ack_clipboard_message (ack, sequence));
}

/* Launches the desktop application with that name or id (--start-app). */
static void
scrcpy_start_app (const char *name, void *user_data)
{
  g_autofree char *wanted = g_utf8_casefold (name, -1);
  g_autofree char *wanted_id = g_strconcat (name, ".desktop", NULL);
  GList *apps = g_app_info_get_all ();
  GAppInfo *match = NULL;
  g_autoptr (GError) error = NULL;

  for (GList *l = apps; l && !match; l = l->next)
    {
      GAppInfo *app = l->data;
      g_autofree char *label = g_utf8_casefold (g_app_info_get_display_name (app), -1);
      const char *id = g_app_info_get_id (app);

      if (!g_strcmp0 (label, wanted) || !g_strcmp0 (id, name) || !g_strcmp0 (id, wanted_id))
        match = app;
    }
  if (!match)
    fprintf (stderr, "xscreend: scrcpy: no application named '%s'\n", name);
  else if (!g_app_info_launch (match, NULL, NULL, &error))
    fprintf (stderr, "xscreend: scrcpy: cannot start '%s': %s\n", name, error->message);
  g_list_free_full (apps, g_object_unref);
}

static gboolean
on_scrcpy_control_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  ssize_t n = recv (fd, server.control_buffer + server.control_fill,
                    sizeof server.control_buffer - server.control_fill, 0);
  long used;

  if (n <= 0)
    {
      disconnect_client ();
      return G_SOURCE_REMOVE;
    }
  server.control_fill += n;
  used = xs_scrcpy_control_feed (server.scrcpy_control, server.control_buffer,
                                 server.control_fill);
  if (used < 0 || (used == 0 && server.control_fill == sizeof server.control_buffer))
    {
      fprintf (stderr, "xscreend: scrcpy: malformed control stream\n");
      disconnect_client ();
      return G_SOURCE_REMOVE;
    }
  memmove (server.control_buffer, server.control_buffer + used, server.control_fill - used);
  server.control_fill -= used;
  return G_SOURCE_CONTINUE;
}

/* Reads the role line a relayed stream starts with. */
static bool
read_role_line (int fd, char *line, size_t size)
{
  size_t len = 0;

  while (len + 1 < size)
    {
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      char c;

      if (poll (&pfd, 1, 2000) <= 0 || recv (fd, &c, 1, 0) != 1)
        return false;
      if (c == '\n')
        {
          line[len] = '\0';
          return true;
        }
      line[len++] = c;
    }
  return false;
}

static void
attach_scrcpy_video (int fd, const char *session, int width, int height, bool cursor)
{
  uint32_t codec = htobe32 (XS_SCRCPY_CODEC_H265);
  XsScrcpyCallbacks callbacks = {
    .input = scrcpy_input,
    .keyframe = scrcpy_keyframe,
    .get_clipboard = scrcpy_get_clipboard,
    .set_clipboard = scrcpy_set_clipboard,
    .start_app = scrcpy_start_app,
  };
  XsMonitorSpec spec = { .vendor = "scrcpy", .product = "scrcpy" };
  char serial[32];
  g_autoptr (GError) error = NULL;

  if (server.client_fd >= 0)
    {
      /* One client at a time: the scrcpy client reports a configuration
       * error and exits instead of taking over the running session. */
      codec = htobe32 (XS_SCRCPY_CODEC_ERROR);
      write_all (fd, &codec, sizeof codec);
      close (fd);
      fprintf (stderr, "xscreend: scrcpy refused: another client is connected\n");
      return;
    }
  if (width < 640 || height < 480)
    {
      width = 1920;
      height = 1080;
    }
  /* GNOME remembers a monitor's mode by its identity; one per requested
   * size keeps a remembered mode from overriding the size asked for. */
  g_snprintf (serial, sizeof serial, "%dx%d", width, height);
  spec.serial = serial;
  /* The requested size first (preferred), then common ones to pick from
   * in GNOME Settings. */
  add_resolution (&spec, width, height, 60.0, 1.0, true);
  for (size_t i = 0; i < G_N_ELEMENTS (standard_modes); i++)
    add_resolution (&spec, standard_modes[i].width, standard_modes[i].height, 60.0, 1.0, false);

  pthread_mutex_lock (&server.write_lock);
  server.client_fd = fd;
  pthread_mutex_unlock (&server.write_lock);
  server.scrcpy = true;
  server.scrcpy_cursor = cursor;
  g_strlcpy (server.scrcpy_session, session, sizeof server.scrcpy_session);
  server.scrcpy_control = xs_scrcpy_control_new (&callbacks, spec.modes[0].width,
                                                 spec.modes[0].height);
  reset_stats ();
  server.client_source = g_unix_fd_source_new (fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
  g_source_set_callback (server.client_source, G_SOURCE_FUNC (on_client_readable), NULL, NULL);
  g_source_attach (server.client_source, NULL);
  fprintf (stderr, "xscreend: scrcpy client connected (%dx%d)\n",
           spec.modes[0].width, spec.modes[0].height);

  if (!write_all (fd, &codec, sizeof codec) ||
      !start_desktop_session (&spec, spec.modes[0].width, spec.modes[0].height, &error))
    {
      fprintf (stderr, "xscreend: scrcpy: %s\n", error ? error->message : "connection lost");
      disconnect_client ();
      return;
    }
  server.clipboard = xs_clipboard_new (server.bus, server.rd_session_path,
                                       on_clipboard_changed, NULL, &error);
  if (!server.clipboard)
    fprintf (stderr, "xscreend: scrcpy: no clipboard sharing: %s\n", error->message);
}

static gboolean
on_scrcpy_listen_readable (gint fd, GIOCondition condition, gpointer user_data)
{
  int client = accept4 (fd, NULL, NULL, SOCK_CLOEXEC);
  int one = 1, width = 0, height = 0;
  bool cursor;
  char line[256], role[16], session[40];
  const char *size;

  if (client < 0)
    return G_SOURCE_CONTINUE;
  if (!peer_allowed (client) || !read_role_line (client, line, sizeof line) ||
      sscanf (line, "XSCRCPY1 %15s %39s", role, session) != 2)
    {
      close (client);
      return G_SOURCE_CONTINUE;
    }
  if ((size = strstr (line, " size=")))
    sscanf (size, " size=%dx%d", &width, &height);
  cursor = strstr (line, " cursor") != NULL;
  setsockopt (client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  if (!strcmp (role, "video"))
    attach_scrcpy_video (client, session, width, height, cursor);
  else if (server.scrcpy && !strcmp (session, server.scrcpy_session) &&
           !strcmp (role, "audio") && server.scrcpy_audio_fd < 0)
    {
      /* No desktop audio yet: the client carries on without it. */
      uint32_t codec = htobe32 (XS_SCRCPY_CODEC_DISABLED);

      write_all (client, &codec, sizeof codec);
      server.scrcpy_audio_fd = client;
    }
  else if (server.scrcpy && !strcmp (session, server.scrcpy_session) &&
           !strcmp (role, "control") && server.scrcpy_control_fd < 0)
    {
      server.scrcpy_control_fd = client;
      server.scrcpy_control_source = g_unix_fd_source_new (client, G_IO_IN | G_IO_HUP | G_IO_ERR);
      g_source_set_callback (server.scrcpy_control_source,
                             G_SOURCE_FUNC (on_scrcpy_control_readable), NULL, NULL);
      g_source_attach (server.scrcpy_control_source, NULL);
    }
  else
    close (client);
  return G_SOURCE_CONTINUE;
}

static int
listen_on (int port)
{
  struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons (port),
    .sin_addr.s_addr = htonl (INADDR_LOOPBACK),
  };
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;

  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind (fd, (struct sockaddr *) &addr, sizeof addr) < 0 || listen (fd, 4) < 0)
    {
      fprintf (stderr, "xscreend: cannot listen on 127.0.0.1:%d: %s\n", port, strerror (errno));
      return -1;
    }
  return fd;
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  int listen_fd, scrcpy_fd;

  server.port = XS_DEFAULT_PORT;
  server.client_fd = -1;
  server.scrcpy_audio_fd = -1;
  server.scrcpy_control_fd = -1;
  server.bitrate = 40000000;
  pthread_mutex_init (&server.lock, NULL);
  pthread_mutex_init (&server.write_lock, NULL);
  timing.enabled = getenv ("XS_DEBUG_TIMING") != NULL;

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--port") && i + 1 < argc)
        server.port = atoi (argv[++i]);
      else if (!strcmp (argv[i], "--raw") && i + 1 < argc &&
               sscanf (argv[++i], "%dx%d", &server.raw_width, &server.raw_height) == 2)
        server.raw = true;
      else
        {
          fprintf (stderr, "usage: xscreend [--port N] [--raw WIDTHxHEIGHT]\n");
          return 2;
        }
    }

  server.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!server.bus)
    {
      fprintf (stderr, "xscreend: session bus: %s\n", error->message);
      return 1;
    }

  listen_fd = listen_on (server.port);
  if (listen_fd < 0)
    return 1;
  g_unix_fd_add (listen_fd, G_IO_IN, on_listen_readable, NULL);
  scrcpy_fd = listen_on (XS_SCRCPY_PORT);
  if (scrcpy_fd >= 0)
    g_unix_fd_add (scrcpy_fd, G_IO_IN, on_scrcpy_listen_readable, NULL);
  server.monitors_changed_id =
    g_dbus_connection_signal_subscribe (server.bus, "org.gnome.Mutter.DisplayConfig",
                                        "org.gnome.Mutter.DisplayConfig", "MonitorsChanged",
                                        "/org/gnome/Mutter/DisplayConfig", NULL, 0,
                                        on_monitors_changed, NULL, NULL);
  export_daemon ();
  g_timeout_add (50, on_refine_timer, NULL);
  g_timeout_add_seconds (5, on_stats_timer, NULL);

  fprintf (stderr, "xscreend: listening on 127.0.0.1:%d%s\n", server.port,
           server.raw ? " (raw Annex-B)" : "");
  g_main_loop_run (loop);
  return 0;
}
