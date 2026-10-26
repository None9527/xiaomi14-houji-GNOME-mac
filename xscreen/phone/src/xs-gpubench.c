/*
 * xs-gpubench: where does the BGRx -> NV12 conversion spend its time?
 *
 * Grabs one frame from a mutter virtual monitor and converts it repeatedly,
 * reporting CPU submit time, fence wait, per-pass GPU time (timer queries)
 * and the GPU clock, first paced at 60 Hz and then back to back.
 *
 * usage: xs-gpubench [width height [iterations]]
 */
#include <gio/gio.h>
#include <poll.h>
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
  int width;
  int height;
  int iterations;
  atomic_bool done;
} Bench;

static int64_t
now_ns (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static long
gpu_freq_mhz (void)
{
  FILE *f = fopen ("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "r");
  long hz = 0;

  if (f)
    {
      if (fscanf (f, "%ld", &hz) != 1)
        hz = 0;
      fclose (f);
    }
  return hz / 1000000;
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

  qsort (v, n, sizeof *v, cmp64);
  for (int i = 0; i < n; i++)
    sum += v[i];
  printf ("  %-10s avg %6.3f ms  p50 %6.3f  p95 %6.3f\n", name,
          sum / (double) n / 1e6, v[n / 2] / 1e6, v[n * 95 / 100] / 1e6);
}

static void
run (XsGpu *gpu, Bench *bench, const XsGpuSource *source, bool paced)
{
  int n = bench->iterations;
  int64_t *submit = calloc (n, sizeof *submit);
  int64_t *wait = calloc (n, sizeof *wait);
  int64_t *total = calloc (n, sizeof *total);
  int64_t *gpu_ns = calloc (n, sizeof *gpu_ns);
  int64_t *c_setup = calloc (n, sizeof *c_setup);
  int64_t *c_dispatch = calloc (n, sizeof *c_dispatch);
  int64_t *c_flush = calloc (n, sizeof *c_flush);
  int64_t *c_fence = calloc (n, sizeof *c_fence);
  long freq_min = 1 << 30, freq_max = 0;

  for (int i = 0; i < n; i++)
    {
      const XsEncInput *in = xs_enc_get_input (bench->enc, i % xs_enc_get_n_inputs (bench->enc));
      XsGpuTarget target = {
        .fd = in->dmabuf_fd, .width = bench->width, .height = bench->height,
        .stride = in->stride, .uv_offset = in->uv_offset,
      };
      XsGpuTimings timings = { 0 };
      int64_t t0 = now_ns (), t1, t2;
      int fence;
      long mhz;

      fence = xs_gpu_convert (gpu, source, &target, NULL);
      t1 = now_ns ();
      if (fence >= 0)
        {
          struct pollfd pfd = { .fd = fence, .events = POLLIN };

          poll (&pfd, 1, 1000);
          close (fence);
        }
      t2 = now_ns ();
      xs_gpu_read_timings (gpu, &timings);

      submit[i] = t1 - t0;
      wait[i] = t2 - t1;
      total[i] = t2 - t0;
      gpu_ns[i] = timings.convert_ns;
      c_setup[i] = timings.cpu_setup_ns;
      c_dispatch[i] = timings.cpu_dispatch_ns;
      c_flush[i] = timings.cpu_flush_ns;
      c_fence[i] = timings.cpu_fence_ns;
      mhz = gpu_freq_mhz ();
      if (mhz && mhz < freq_min)
        freq_min = mhz;
      if (mhz > freq_max)
        freq_max = mhz;

      if (paced && t2 - t0 < 16666667)
        usleep ((16666667 - (t2 - t0)) / 1000);
    }

  printf ("%s, %d iterations, GPU clock %ld-%ld MHz\n",
          paced ? "paced 60 Hz" : "back to back", n, freq_min, freq_max);
  report ("submit", submit, n);
  report ("fence", wait, n);
  report ("total", total, n);
  report ("gpu", gpu_ns, n);
  report (" .setup", c_setup, n);
  report (" .dispatch", c_dispatch, n);
  report (" .flush", c_flush, n);
  report (" .dupfence", c_fence, n);
  free (submit); free (wait); free (total); free (gpu_ns);
  free (c_setup); free (c_dispatch); free (c_flush); free (c_fence);
}

static void
on_frame (const XsGpuSource *source, const XsRegion *damage, int64_t pts_us,
          void *user_data)
{
  Bench *bench = user_data;
  char error[256];
  XsGpu *gpu;

  if (atomic_load (&bench->done))
    return;
  gpu = xs_gpu_new (error, sizeof error);
  if (!gpu)
    {
      fprintf (stderr, "gpu: %s\n", error);
      atomic_store (&bench->done, true);
      return;
    }
  xs_gpu_set_profiling (gpu, true);

  /* Warm-up: imports and shader compilation happen on first use. */
  for (int i = 0; i < xs_enc_get_n_inputs (bench->enc); i++)
    {
      const XsEncInput *in = xs_enc_get_input (bench->enc, i);
      XsGpuTarget target = {
        .fd = in->dmabuf_fd, .width = bench->width, .height = bench->height,
        .stride = in->stride, .uv_offset = in->uv_offset,
      };
      int fence = xs_gpu_convert (gpu, source, &target, NULL);

      if (fence >= 0)
        {
          struct pollfd pfd = { .fd = fence, .events = POLLIN };

          poll (&pfd, 1, 1000);
          close (fence);
        }
    }

  run (gpu, bench, source, true);
  run (gpu, bench, source, false);
  xs_gpu_free (gpu);
  atomic_store (&bench->done, true);
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = NULL;
  Bench bench = { 0 };
  XsCaptureCallbacks callbacks = { .frame = on_frame, .user_data = &bench };
  XsCapture *capture;
  XsEncConfig config;
  char enc_error[256];

  bench.width = argc > 2 ? atoi (argv[1]) : 2560;
  bench.height = argc > 2 ? atoi (argv[2]) : 1600;
  bench.iterations = argc > 3 ? atoi (argv[3]) : 200;

  config = (XsEncConfig) {
    .device = "/dev/video33", .codec = XS_CODEC_HEVC,
    .width = bench.width, .height = bench.height,
    .fps = 60, .bitrate = 40000000, .n_inputs = 4,
  };
  bench.enc = xs_enc_new (&config, enc_error, sizeof enc_error);
  if (!bench.enc)
    {
      fprintf (stderr, "encoder: %s\n", enc_error);
      return 1;
    }

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  capture = bus ? xs_capture_new (bus, NULL, NULL, bench.width, bench.height, 60, false, &callbacks, &error)
                : NULL;
  if (!capture || !xs_capture_start (capture, true, &error))
    {
      fprintf (stderr, "capture: %s\n", error ? error->message : "?");
      return 1;
    }

  while (!atomic_load (&bench.done))
    if (!g_main_context_iteration (NULL, FALSE))
      usleep (10000);

  xs_capture_free (capture);
  xs_enc_free (bench.enc);
  return 0;
}
