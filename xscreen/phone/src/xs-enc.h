/*
 * xs-enc: low-latency HEVC/H.264 encoding on a V4L2 stateful encoder
 * (Qualcomm msm_vidc / VPU).
 *
 * The NV12 input buffers are dmabufs allocated from a dma-heap and imported
 * by the encoder, so the GPU can render the converted frame straight into
 * the memory the VPU reads.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XS_ENC_MAX_INPUTS 8

typedef enum
{
  XS_CODEC_HEVC = 1,
  XS_CODEC_H264 = 2,
} XsCodec;

typedef struct
{
  const char *device;      /* "/dev/video33" */
  XsCodec codec;
  int width;
  int height;
  int fps;                 /* nominal rate, used for rate control only */
  int bitrate;             /* bits per second */
  int n_inputs;            /* 2..XS_ENC_MAX_INPUTS */
  /* QP caps, lower is better quality; 0 keeps the driver default. The VPU
   * only accepts them before streaming starts. */
  int i_frame_max_qp;
  int p_frame_max_qp;
} XsEncConfig;

/* NV12 layout of one encoder input buffer. */
typedef struct
{
  int dmabuf_fd;
  uint32_t stride;         /* bytes per row, both planes */
  uint32_t uv_offset;      /* byte offset of the interleaved CbCr plane */
  uint32_t size;
} XsEncInput;

typedef void (*XsEncOutputFunc) (const uint8_t *data,
                                 size_t         size,
                                 int64_t        pts_us,
                                 bool           keyframe,
                                 void          *user_data);

typedef struct _XsEnc XsEnc;

XsEnc *xs_enc_new (const XsEncConfig *config,
                   char              *error,
                   size_t             error_size);

void xs_enc_free (XsEnc *enc);

int xs_enc_get_n_inputs (XsEnc *enc);

const XsEncInput *xs_enc_get_input (XsEnc *enc,
                                    int    index);

/* Returns the index of an input buffer the hardware is not using, waiting
 * up to `timeout_ms` (< 0: forever). Returns -1 on timeout or error. */
int xs_enc_acquire_input (XsEnc *enc,
                          int    timeout_ms);

/* Whether input `index` is currently not owned by the hardware. */
bool xs_enc_input_is_free (XsEnc *enc,
                           int    index);

bool xs_enc_queue_input (XsEnc   *enc,
                         int      index,
                         int64_t  pts_us,
                         bool     force_keyframe);

/* Delivers every finished access unit to `func`. `timeout_ms` < 0 waits
 * forever. Returns the number delivered, or -1 on a fatal error. */
int xs_enc_poll_output (XsEnc           *enc,
                        int              timeout_ms,
                        XsEncOutputFunc  func,
                        void            *user_data);
