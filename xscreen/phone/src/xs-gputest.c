/*
 * xs-gputest: checks the GPU BGRx -> NV12 conversion against a CPU
 * reference, for a full frame and for a partial (damage) update.
 *
 * usage: xs-gputest [width height]
 */
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>

#include "xs-gpu.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

typedef struct
{
  int fd;
  uint8_t *data;
  size_t size;
} Buffer;

static bool
alloc_buffer (Buffer *b, size_t size)
{
  struct dma_heap_allocation_data alloc = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
  int heap = open ("/dev/dma_heap/qcom,system", O_RDONLY | O_CLOEXEC);

  if (heap < 0)
    heap = open ("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
  if (heap < 0 || ioctl (heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0)
    return false;
  close (heap);
  b->fd = alloc.fd;
  b->size = size;
  b->data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
  return b->data != MAP_FAILED;
}

static void
cpu_access (Buffer *b, bool start, bool write)
{
  struct dma_buf_sync sync = {
    .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
             (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
  };

  ioctl (b->fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* BGRx in memory: B, G, R, X. */
static void
fill (Buffer *b, int width, int height, int seed)
{
  cpu_access (b, true, true);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        uint8_t *p = b->data + ((size_t) y * width + x) * 4;
        uint32_t h = (uint32_t) (x * 73856093u) ^ (uint32_t) (y * 19349663u) ^ (uint32_t) seed;

        h ^= h >> 13;
        h *= 0x5bd1e995u;
        p[0] = seed ? h : (x + y) & 0xff;
        p[1] = seed ? h >> 8 : y & 0xff;
        p[2] = seed ? h >> 16 : x & 0xff;
        p[3] = 0xff;
      }
  cpu_access (b, false, true);
}

/* Same math as the shader, in float. */
static void
reference (const Buffer *src, int width, int height, uint8_t *y_plane, uint8_t *uv_plane)
{
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        const uint8_t *p = src->data + ((size_t) y * width + x) * 4;
        float l = (0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0]) / 255.0f;

        y_plane[(size_t) y * width + x] = (uint8_t) lrintf ((16.0f / 255.0f + l * 219.0f / 255.0f) * 255.0f);
      }
  for (int y = 0; y < height / 2; y++)
    for (int x = 0; x < width / 2; x++)
      {
        float r = 0, g = 0, bl = 0, l, cb, cr;

        for (int dy = 0; dy < 2; dy++)
          for (int dx = 0; dx < 2; dx++)
            {
              const uint8_t *p = src->data + ((size_t) (2 * y + dy) * width + 2 * x + dx) * 4;

              r += p[2] / 255.0f; g += p[1] / 255.0f; bl += p[0] / 255.0f;
            }
        r /= 4; g /= 4; bl /= 4;
        l = 0.2126f * r + 0.7152f * g + 0.0722f * bl;
        cb = 128.0f / 255.0f + (bl - l) * (224.0f / 255.0f / 1.8556f);
        cr = 128.0f / 255.0f + (r - l) * (224.0f / 255.0f / 1.5748f);
        uv_plane[(size_t) y * width + 2 * x] = (uint8_t) lrintf (cb * 255.0f);
        uv_plane[(size_t) y * width + 2 * x + 1] = (uint8_t) lrintf (cr * 255.0f);
      }
}

static bool
in_blocks (const XsRegion *region, int x, int y)
{
  for (int i = 0; i < region->n_rects; i++)
    {
      const XsRect *r = &region->rects[i];
      int x0 = r->x / 4 * 4, y0 = r->y / 2 * 2;
      int x1 = (r->x + r->width + 3) / 4 * 4, y1 = (r->y + r->height + 1) / 2 * 2;

      if (x >= x0 && x < x1 && y >= y0 && y < y1)
        return true;
    }
  return false;
}

/* Compares the NV12 target with `expect_in` inside the region's blocks and
 * `expect_out` elsewhere (`region` NULL: everything is "in"). */
static int
compare (Buffer *target, int width, int height, uint32_t stride, uint32_t uv_offset,
         const XsRegion *region,
         const uint8_t *y_in, const uint8_t *uv_in,
         const uint8_t *y_out, const uint8_t *uv_out)
{
  int bad = 0, worst = 0;

  cpu_access (target, true, false);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        bool in = !region || in_blocks (region, x, y);
        int got = target->data[(size_t) y * stride + x];
        int want = (in ? y_in : y_out)[(size_t) y * width + x];
        int d = abs (got - want);

        bad += d > 1;
        worst = d > worst ? d : worst;
      }
  for (int y = 0; y < height / 2; y++)
    for (int x = 0; x < width; x++)
      {
        bool in = !region || in_blocks (region, x, y * 2);
        int got = target->data[uv_offset + (size_t) y * stride + x];
        int want = (in ? uv_in : uv_out)[(size_t) y * width + x];
        int d = abs (got - want);

        bad += d > 1;
        worst = d > worst ? d : worst;
      }
  cpu_access (target, false, false);
  printf ("  mismatches (>1): %d, worst difference: %d\n", bad, worst);
  return bad;
}

static void
wait_fence (int fence)
{
  if (fence >= 0)
    {
      struct pollfd pfd = { .fd = fence, .events = POLLIN };

      poll (&pfd, 1, 2000);
      close (fence);
    }
}

int
main (int argc, char **argv)
{
  int width = argc > 2 ? atoi (argv[1]) : 2560;
  int height = argc > 2 ? atoi (argv[2]) : 1600;
  uint32_t stride = ALIGN (width, 128);
  uint32_t uv_offset = stride * ALIGN (height, 32);
  Buffer a, b, target;
  uint8_t *ya = malloc ((size_t) width * height), *uva = malloc ((size_t) width * height / 2);
  uint8_t *yb = malloc ((size_t) width * height), *uvb = malloc ((size_t) width * height / 2);
  char error[256];
  XsGpu *gpu;
  XsRegion region;
  int failures = 0;

  if (!alloc_buffer (&a, (size_t) width * height * 4) ||
      !alloc_buffer (&b, (size_t) width * height * 4) ||
      !alloc_buffer (&target, uv_offset + (size_t) stride * height / 2))
    {
      fprintf (stderr, "dma-heap allocation failed\n");
      return 1;
    }
  gpu = xs_gpu_new (error, sizeof error);
  if (!gpu)
    {
      fprintf (stderr, "gpu: %s\n", error);
      return 1;
    }

  fill (&a, width, height, 0);
  fill (&b, width, height, 12345);
  cpu_access (&a, true, false);
  reference (&a, width, height, ya, uva);
  cpu_access (&a, false, false);
  cpu_access (&b, true, false);
  reference (&b, width, height, yb, uvb);
  cpu_access (&b, false, false);

  {
    XsGpuSource src_a = { a.fd, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, width, height, 0, width * 4 };
    XsGpuSource src_b = { b.fd, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, width, height, 0, width * 4 };
    XsGpuTarget dst = { target.fd, width, height, stride, uv_offset };

    printf ("full conversion of A (%dx%d):\n", width, height);
    wait_fence (xs_gpu_convert (gpu, &src_a, &dst, NULL));
    failures += compare (&target, width, height, stride, uv_offset, NULL, ya, uva, NULL, NULL);

    /* Unaligned rectangles, one touching the right and bottom edges. */
    xs_region_clear (&region);
    xs_region_add (&region, &(XsRect) { 101, 37, 333, 211 }, width, height);
    xs_region_add (&region, &(XsRect) { width - 50, height - 9, 80, 40 }, width, height);
    printf ("partial conversion of B into A (2 rects):\n");
    wait_fence (xs_gpu_convert (gpu, &src_b, &dst, &region));
    failures += compare (&target, width, height, stride, uv_offset, &region, yb, uvb, ya, uva);

    printf ("empty region is a no-op: %s\n",
            xs_gpu_convert (gpu, &src_a, &dst, &(XsRegion) { .n_rects = 0 }) == -1 ? "yes" : "NO");
  }

  xs_gpu_free (gpu);
  printf (failures ? "FAILED\n" : "PASSED\n");
  return failures != 0;
}
