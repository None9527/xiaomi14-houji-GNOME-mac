/*
 * xs-probe: end-to-end check of the phone half of the pipeline.
 *
 *   mutter virtual monitor --dmabuf--> GPU (BGRx -> NV12, into encoder
 *   buffers) --> VPU HEVC --> file
 *
 * usage: xs-probe [width height [frames [out.hevc]]]
 */
#include <gio/gio.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "xs-capture.h"
#include "xs-enc.h"
#include "xs-gpu.h"

typedef struct
{
  XsEnc *enc;
  XsGpu *gpu;
  FILE *out;
  int target_frames;
  int width;
  int height;

  atomic_int submitted;
  atomic_int encoded;
  atomic_bool done;
  int64_t submit_us[4096];
  int64_t gpu_us[4096];
  int64_t encode_us[4096];
  size_t bytes;
  int keyframes;
} Probe;

static int64_t
now_us (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
on_output (const uint8_t *data, size_t size, int64_t pts_us, bool keyframe, void *user_data)
{
  Probe *probe = user_data;
  int i = atomic_load (&probe->encoded);

  if (i < 4096)
    probe->encode_us[i] = now_us () - pts_us;
  fwrite (data, 1, size, probe->out);
  probe->bytes += size;
  probe->keyframes += keyframe;
  atomic_fetch_add (&probe->encoded, 1);
}

static void *
output_thread (void *data)
{
  Probe *probe = data;

  while (!atomic_load (&probe->done) || atomic_load (&probe->encoded) < atomic_load (&probe->submitted))
    {
      if (xs_enc_poll_output (probe->enc, 100, on_output, probe) < 0)
        break;
      if (atomic_load (&probe->done) && atomic_load (&probe->encoded) >= atomic_load (&probe->submitted))
        break;
    }
  return NULL;
}

/* Runs on the PipeWire thread: the GL context lives here. */
static void
on_frame (const XsGpuSource *source, const XsRegion *damage, int64_t pts_us,
          void *user_data)
{
  Probe *probe = user_data;
  char error[256];

  if (atomic_load (&probe->done))
    return;

  if (!probe->gpu)
    {
      probe->gpu = xs_gpu_new (error, sizeof error);
      if (!probe->gpu)
        {
          fprintf (stderr, "gpu: %s\n", error);
          atomic_store (&probe->done, true);
          return;
        }
    }

  printf ("source: %dx%d stride=%u modifier=0x%llx\n", source->width, source->height,
          source->stride, (unsigned long long) source->modifier);

  /* Re-encode the same frame repeatedly at 60 Hz: the virtual monitor
   * only produces frames on damage, and this measures the steady state. */
  for (int n = 0; n < probe->target_frames; n++)
    {
      int64_t t0 = now_us ();
      int index = xs_enc_acquire_input (probe->enc, 1000);
      const XsEncInput *input;
      XsGpuTarget target;
      int fence;

      if (index < 0)
        {
          fprintf (stderr, "no free encoder input\n");
          break;
        }
      input = xs_enc_get_input (probe->enc, index);
      target = (XsGpuTarget) {
        .fd = input->dmabuf_fd,
        .width = probe->width,
        .height = probe->height,
        .stride = input->stride,
        .uv_offset = input->uv_offset,
      };

      t0 = now_us ();
      fence = xs_gpu_convert (probe->gpu, source, &target, NULL);
      if (fence == -2)
        {
          fprintf (stderr, "conversion failed\n");
          break;
        }
      if (fence >= 0)
        {
          struct pollfd pfd = { .fd = fence, .events = POLLIN };

          poll (&pfd, 1, 1000);
          close (fence);
        }
      probe->gpu_us[n] = now_us () - t0;

      xs_enc_queue_input (probe->enc, index, now_us (), n == 0);
      atomic_fetch_add (&probe->submitted, 1);

      {
        int64_t elapsed = now_us () - t0;

        if (elapsed < 16667)
          usleep (16667 - elapsed);
      }
    }

  atomic_store (&probe->done, true);
}

static int
cmp64 (const void *a, const void *b)
{
  int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;

  return x < y ? -1 : x > y;
}

static void
report (const char *name, int64_t *v, int n)
{
  int64_t sum = 0;

  if (n <= 0)
    return;
  qsort (v, n, sizeof *v, cmp64);
  for (int i = 0; i < n; i++)
    sum += v[i];
  printf ("%-8s avg %6.2f ms  p50 %6.2f  p95 %6.2f  max %6.2f\n", name,
          sum / (double) n / 1000.0, v[n / 2] / 1000.0, v[n * 95 / 100] / 1000.0,
          v[n - 1] / 1000.0);
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = NULL;
  Probe probe = { 0 };
  XsCapture *capture;
  XsCaptureCallbacks callbacks = { .frame = on_frame, .user_data = &probe };
  XsEncConfig config;
  char enc_error[256];
  pthread_t thread;
  const char *out_path = argc > 4 ? argv[4] : "/tmp/xs-probe.hevc";

  probe.width = argc > 2 ? atoi (argv[1]) : 2560;
  probe.height = argc > 2 ? atoi (argv[2]) : 1600;
  probe.target_frames = argc > 3 ? atoi (argv[3]) : 120;
  if (probe.target_frames > 4096)
    probe.target_frames = 4096;

  config = (XsEncConfig) {
    .device = "/dev/video33",
    .codec = XS_CODEC_HEVC,
    .width = probe.width,
    .height = probe.height,
    .fps = 60,
    .bitrate = 40000000,
    .n_inputs = 4,
  };
  probe.enc = xs_enc_new (&config, enc_error, sizeof enc_error);
  if (!probe.enc)
    {
      fprintf (stderr, "encoder: %s\n", enc_error);
      return 1;
    }
  {
    const XsEncInput *in = xs_enc_get_input (probe.enc, 0);

    printf ("encoder: %d inputs, NV12 stride=%u uv_offset=%u size=%u\n",
            xs_enc_get_n_inputs (probe.enc), in->stride, in->uv_offset, in->size);
  }

  probe.out = fopen (out_path, "wb");
  pthread_create (&thread, NULL, output_thread, &probe);

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  capture = bus ? xs_capture_new (bus, NULL, NULL, probe.width, probe.height, 60, false, &callbacks, &error)
                : NULL;
  if (!capture || !xs_capture_start (capture, true, &error))
    {
      fprintf (stderr, "capture: %s\n", error ? error->message : "?");
      atomic_store (&probe.done, true);
    }

  while (!atomic_load (&probe.done))
    if (!g_main_context_iteration (NULL, FALSE))
      usleep (10000);

  pthread_join (thread, NULL);
  xs_capture_free (capture);
  fclose (probe.out);

  printf ("frames: submitted %d, encoded %d, keyframes %d, %.1f KiB total\n",
          atomic_load (&probe.submitted), atomic_load (&probe.encoded),
          probe.keyframes, probe.bytes / 1024.0);
  report ("gpu", probe.gpu_us, atomic_load (&probe.submitted));
  report ("encode", probe.encode_us, atomic_load (&probe.encoded));

  xs_enc_free (probe.enc);
  return 0;
}
