/*
 * xs-gpu: GPU colour conversion from a BGRx/BGRA dmabuf (as produced by a
 * mutter screen cast) into NV12 dmabufs owned by the encoder.
 *
 * One compute dispatch on an EGL surfaceless GLES 3.1 context; nothing
 * touches the CPU. Frame sizes must be multiples of 4x2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xs-damage.h"

typedef struct _XsGpu XsGpu;

typedef struct
{
  int fd;
  uint32_t drm_format;     /* DRM_FORMAT_XRGB8888 or DRM_FORMAT_ARGB8888 */
  uint64_t modifier;
  int width;
  int height;
  uint32_t offset;
  uint32_t stride;
} XsGpuSource;

typedef struct
{
  int fd;
  int width;
  int height;
  uint32_t stride;
  uint32_t uv_offset;
} XsGpuTarget;

XsGpu *xs_gpu_new (char   *error,
                   size_t  error_size);

void xs_gpu_free (XsGpu *gpu);

/* Imports are cached by (fd, offset); PipeWire recycles a small fixed set
 * of buffers, so after warm-up no EGL images are created per frame.
 * Call when the stream renegotiates and its buffers go away. */
void xs_gpu_forget_sources (XsGpu *gpu);

void xs_gpu_forget_targets (XsGpu *gpu);

typedef struct
{
  uint64_t convert_ns;     /* GPU execution time of the conversion */
  /* CPU time of the submission phases of the last xs_gpu_convert(). */
  uint64_t cpu_setup_ns;
  uint64_t cpu_dispatch_ns;
  uint64_t cpu_flush_ns;
  uint64_t cpu_fence_ns;
} XsGpuTimings;

/* Enables GPU timer queries; xs_gpu_read_timings() then reports the most
 * recent conversion once its fence has signalled. */
void xs_gpu_set_profiling (XsGpu *gpu,
                           bool   enabled);

bool xs_gpu_read_timings (XsGpu        *gpu,
                          XsGpuTimings *timings);

/* Converts the part of `source` covered by `region` into `target`; NULL
 * or a full region converts everything. On success returns a sync_file fd
 * that signals when the GPU is done writing (the caller owns it), or -1 if
 * the work already completed or there was nothing to do. Returns -2 on
 * failure. */
int xs_gpu_convert (XsGpu             *gpu,
                    const XsGpuSource *source,
                    const XsGpuTarget *target,
                    const XsRegion    *region);
