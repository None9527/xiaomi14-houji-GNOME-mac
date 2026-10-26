#include "xs-enc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Heaps tried in order for the NV12 input buffers; the first is what the
 * Android gralloc uses for non-secure video buffers on Snapdragon. */
static const char *dma_heaps[] = {
  "/dev/dma_heap/qcom,system",
  "/dev/dma_heap/system",
};

/* Per-frame-type QP caps; msm_vidc only exposes the set matching the coded
 * format (V4L2_CID_MPEG_VIDEO_{H264,HEVC}_{I,P}_FRAME_MAX_QP). */
#define XS_CID_H264_I_FRAME_MAX_QP 0x00990a82
#define XS_CID_H264_P_FRAME_MAX_QP 0x00990a84
#define XS_CID_HEVC_I_FRAME_MAX_QP 0x00990b88
#define XS_CID_HEVC_P_FRAME_MAX_QP 0x00990b8a

/* msm_vidc vendor control: 1-in-1-out pipeline without lookahead. */
#define XS_CID_MSM_VIDC_LOWLATENCY 0x00992003

#define XS_ENC_N_OUTPUTS 4
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

typedef struct
{
  int fd;
  void *data;
  size_t length;
} XsEncOutputBuffer;

struct _XsEnc
{
  int fd;
  XsCodec codec;
  int width;
  int height;

  pthread_mutex_t lock;

  int n_inputs;
  XsEncInput inputs[XS_ENC_MAX_INPUTS];
  bool input_busy[XS_ENC_MAX_INPUTS];
  int n_busy;

  XsEncOutputBuffer outputs[XS_ENC_N_OUTPUTS];
};

static void
set_error (char *error, size_t error_size, const char *fmt, ...)
{
  va_list args;

  if (!error || !error_size)
    return;
  va_start (args, fmt);
  vsnprintf (error, error_size, fmt, args);
  va_end (args);
}

static int
xioctl (int fd, unsigned long request, void *arg)
{
  int ret;

  do
    ret = ioctl (fd, request, arg);
  while (ret == -1 && errno == EINTR);
  return ret;
}

static bool
set_ctrl (XsEnc *enc, uint32_t id, int32_t value, bool required)
{
  struct v4l2_control ctrl = { .id = id, .value = value };

  if (xioctl (enc->fd, VIDIOC_S_CTRL, &ctrl) == 0)
    return true;
  if (required)
    fprintf (stderr, "xs-enc: control 0x%08x=%d failed: %s\n",
             id, value, strerror (errno));
  return !required;
}

static bool
configure_formats (XsEnc *enc, const XsEncConfig *config, char *error, size_t error_size)
{
  struct v4l2_format fmt;
  struct v4l2_streamparm parm;
  uint32_t y_scanlines;
  uint32_t uv_size;

  /* The coded format has to be set first; it decides which raw formats the
   * driver offers on the input side. */
  memset (&fmt, 0, sizeof fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = config->width;
  fmt.fmt.pix_mp.height = config->height;
  fmt.fmt.pix_mp.pixelformat = config->codec == XS_CODEC_HEVC ? V4L2_PIX_FMT_HEVC
                                                              : V4L2_PIX_FMT_H264;
  fmt.fmt.pix_mp.num_planes = 1;
  if (xioctl (enc->fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      set_error (error, error_size, "S_FMT(coded): %s", strerror (errno));
      return false;
    }

  memset (&fmt, 0, sizeof fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = config->width;
  fmt.fmt.pix_mp.height = config->height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.num_planes = 1;
  /* Full-range sRGB primaries would be wrong for video; signal BT.709
   * limited range, which is what the GPU conversion produces. */
  fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
  fmt.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
  fmt.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
  fmt.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
  if (xioctl (enc->fd, VIDIOC_S_FMT, &fmt) < 0)
    {
      set_error (error, error_size, "S_FMT(raw NV12): %s", strerror (errno));
      return false;
    }
  if (xioctl (enc->fd, VIDIOC_G_FMT, &fmt) < 0)
    {
      set_error (error, error_size, "G_FMT(raw): %s", strerror (errno));
      return false;
    }

  enc->width = fmt.fmt.pix_mp.width;
  enc->height = fmt.fmt.pix_mp.height;

  /* Venus NV12: luma rows are padded to the reported stride and the luma
   * plane to a multiple of 32 lines; chroma follows directly. */
  y_scanlines = ALIGN (enc->height, 32);
  uv_size = fmt.fmt.pix_mp.plane_fmt[0].bytesperline * ALIGN (enc->height / 2, 16);
  for (int i = 0; i < XS_ENC_MAX_INPUTS; i++)
    {
      enc->inputs[i].stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
      enc->inputs[i].uv_offset = enc->inputs[i].stride * y_scanlines;
      enc->inputs[i].size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
      enc->inputs[i].dmabuf_fd = -1;
    }
  if (enc->inputs[0].uv_offset + uv_size > enc->inputs[0].size)
    {
      set_error (error, error_size,
                 "unexpected NV12 layout: stride=%u sizeimage=%u for %dx%d",
                 enc->inputs[0].stride, enc->inputs[0].size,
                 enc->width, enc->height);
      return false;
    }

  memset (&parm, 0, sizeof parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  parm.parm.output.timeperframe.numerator = 1;
  parm.parm.output.timeperframe.denominator = config->fps;
  xioctl (enc->fd, VIDIOC_S_PARM, &parm);

  return true;
}

static bool
configure_controls (XsEnc *enc, const XsEncConfig *config)
{
  bool ok = true;

  ok &= set_ctrl (enc, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, true);
  ok &= set_ctrl (enc, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                  V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, true);
  ok &= set_ctrl (enc, V4L2_CID_MPEG_VIDEO_BITRATE, config->bitrate, true);
  set_ctrl (enc, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, config->bitrate * 2, false);
  /* Desktop content is mostly static: keyframes only on demand. */
  ok &= set_ctrl (enc, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0x7fffffff, true);
  ok &= set_ctrl (enc, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, true);
  set_ctrl (enc, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, false);
  set_ctrl (enc, XS_CID_MSM_VIDC_LOWLATENCY, 1, false);
  if (config->i_frame_max_qp)
    ok &= set_ctrl (enc, config->codec == XS_CODEC_HEVC ? XS_CID_HEVC_I_FRAME_MAX_QP
                                                        : XS_CID_H264_I_FRAME_MAX_QP,
                    config->i_frame_max_qp, true);
  if (config->p_frame_max_qp)
    ok &= set_ctrl (enc, config->codec == XS_CODEC_HEVC ? XS_CID_HEVC_P_FRAME_MAX_QP
                                                        : XS_CID_H264_P_FRAME_MAX_QP,
                    config->p_frame_max_qp, true);

  if (config->codec == XS_CODEC_HEVC)
    set_ctrl (enc, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
              V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, false);
  else
    set_ctrl (enc, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
              V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, false);

  return ok;
}

static int
alloc_dmabuf (size_t size, char *error, size_t error_size)
{
  for (size_t i = 0; i < sizeof dma_heaps / sizeof dma_heaps[0]; i++)
    {
      struct dma_heap_allocation_data alloc = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
      };
      int heap = open (dma_heaps[i], O_RDONLY | O_CLOEXEC);

      if (heap < 0)
        continue;
      if (xioctl (heap, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0)
        {
          close (heap);
          return alloc.fd;
        }
      close (heap);
    }
  set_error (error, error_size, "no usable dma-heap for %zu bytes: %s", size, strerror (errno));
  return -1;
}

static bool
queue_output (XsEnc *enc, int index)
{
  struct v4l2_plane plane;
  struct v4l2_buffer buf;

  memset (&buf, 0, sizeof buf);
  memset (&plane, 0, sizeof plane);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_DMABUF;
  buf.index = index;
  buf.m.planes = &plane;
  buf.length = 1;
  plane.m.fd = enc->outputs[index].fd;
  plane.length = enc->outputs[index].length;
  return xioctl (enc->fd, VIDIOC_QBUF, &buf) == 0;
}

/* msm_vidc cannot export its own buffers (no VIDIOC_EXPBUF), so the input
 * side imports dmabufs we allocate; the GPU writes into the same memory. */
static bool
setup_buffers (XsEnc *enc, int n_inputs, char *error, size_t error_size)
{
  struct v4l2_requestbuffers req;
  struct v4l2_format fmt;

  memset (&req, 0, sizeof req);
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  req.memory = V4L2_MEMORY_DMABUF;
  req.count = n_inputs;
  if (xioctl (enc->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
    {
      set_error (error, error_size, "REQBUFS(input): %s", strerror (errno));
      return false;
    }
  enc->n_inputs = req.count < XS_ENC_MAX_INPUTS ? req.count : XS_ENC_MAX_INPUTS;

  for (int i = 0; i < enc->n_inputs; i++)
    {
      enc->inputs[i].dmabuf_fd = alloc_dmabuf (enc->inputs[i].size, error, error_size);
      if (enc->inputs[i].dmabuf_fd < 0)
        return false;
    }

  memset (&fmt, 0, sizeof fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl (enc->fd, VIDIOC_G_FMT, &fmt) < 0 || !fmt.fmt.pix_mp.plane_fmt[0].sizeimage)
    {
      set_error (error, error_size, "G_FMT(coded): %s", strerror (errno));
      return false;
    }

  /* Bitstream buffers are dmabufs too: msm_vidc does not mmap its own. */
  memset (&req, 0, sizeof req);
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_DMABUF;
  req.count = XS_ENC_N_OUTPUTS;
  if (xioctl (enc->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < XS_ENC_N_OUTPUTS)
    {
      set_error (error, error_size, "REQBUFS(output): %s", strerror (errno));
      return false;
    }

  for (int i = 0; i < XS_ENC_N_OUTPUTS; i++)
    {
      XsEncOutputBuffer *out = &enc->outputs[i];

      out->length = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
      out->fd = alloc_dmabuf (out->length, error, error_size);
      if (out->fd < 0)
        return false;
      out->data = mmap (NULL, out->length, PROT_READ, MAP_SHARED, out->fd, 0);
      if (out->data == MAP_FAILED)
        {
          out->data = NULL;
          set_error (error, error_size, "mmap(output %d): %s", i, strerror (errno));
          return false;
        }
      if (!queue_output (enc, i))
        {
          set_error (error, error_size, "QBUF(output %d): %s", i, strerror (errno));
          return false;
        }
    }

  return true;
}

XsEnc *
xs_enc_new (const XsEncConfig *config, char *error, size_t error_size)
{
  XsEnc *enc = calloc (1, sizeof *enc);
  int type;

  enc->codec = config->codec;
  pthread_mutex_init (&enc->lock, NULL);
  enc->fd = open (config->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (enc->fd < 0)
    {
      set_error (error, error_size, "open %s: %s", config->device, strerror (errno));
      goto fail;
    }

  if (!configure_formats (enc, config, error, error_size))
    goto fail;
  if (!configure_controls (enc, config))
    {
      set_error (error, error_size, "required encoder controls rejected");
      goto fail;
    }
  if (!setup_buffers (enc, config->n_inputs, error, error_size))
    goto fail;

  type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (xioctl (enc->fd, VIDIOC_STREAMON, &type) < 0)
    {
      set_error (error, error_size, "STREAMON(input): %s", strerror (errno));
      goto fail;
    }
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl (enc->fd, VIDIOC_STREAMON, &type) < 0)
    {
      set_error (error, error_size, "STREAMON(output): %s", strerror (errno));
      goto fail;
    }

  return enc;

fail:
  xs_enc_free (enc);
  return NULL;
}

void
xs_enc_free (XsEnc *enc)
{
  if (!enc)
    return;

  if (enc->fd >= 0)
    {
      int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

      xioctl (enc->fd, VIDIOC_STREAMOFF, &type);
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      xioctl (enc->fd, VIDIOC_STREAMOFF, &type);
    }
  for (int i = 0; i < XS_ENC_N_OUTPUTS; i++)
    {
      if (enc->outputs[i].data)
        munmap (enc->outputs[i].data, enc->outputs[i].length);
      if (enc->outputs[i].fd > 0)
        close (enc->outputs[i].fd);
    }
  for (int i = 0; i < enc->n_inputs; i++)
    if (enc->inputs[i].dmabuf_fd >= 0)
      close (enc->inputs[i].dmabuf_fd);
  if (enc->fd >= 0)
    close (enc->fd);
  pthread_mutex_destroy (&enc->lock);
  free (enc);
}

int
xs_enc_get_n_inputs (XsEnc *enc)
{
  return enc->n_inputs;
}

const XsEncInput *
xs_enc_get_input (XsEnc *enc, int index)
{
  return index >= 0 && index < enc->n_inputs ? &enc->inputs[index] : NULL;
}

/* Called with the lock held. */
static void
reclaim_inputs (XsEnc *enc)
{
  for (;;)
    {
      struct v4l2_plane plane;
      struct v4l2_buffer buf;

      memset (&buf, 0, sizeof buf);
      memset (&plane, 0, sizeof plane);
      buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buf.memory = V4L2_MEMORY_DMABUF;
      buf.m.planes = &plane;
      buf.length = 1;
      if (xioctl (enc->fd, VIDIOC_DQBUF, &buf) < 0)
        return;
      if (buf.index < (uint32_t) enc->n_inputs && enc->input_busy[buf.index])
        {
          enc->input_busy[buf.index] = false;
          enc->n_busy--;
        }
    }
}

int
xs_enc_acquire_input (XsEnc *enc, int timeout_ms)
{
  for (;;)
    {
      struct pollfd pfd = { .fd = enc->fd, .events = POLLOUT };

      pthread_mutex_lock (&enc->lock);
      reclaim_inputs (enc);
      for (int i = 0; i < enc->n_inputs; i++)
        if (!enc->input_busy[i])
          {
            pthread_mutex_unlock (&enc->lock);
            return i;
          }
      pthread_mutex_unlock (&enc->lock);

      if (timeout_ms == 0)
        return -1;
      if (poll (&pfd, 1, timeout_ms) <= 0)
        return -1;
    }
}

bool
xs_enc_input_is_free (XsEnc *enc, int index)
{
  bool is_free;

  if (index < 0 || index >= enc->n_inputs)
    return false;
  pthread_mutex_lock (&enc->lock);
  reclaim_inputs (enc);
  is_free = !enc->input_busy[index];
  pthread_mutex_unlock (&enc->lock);
  return is_free;
}

bool
xs_enc_queue_input (XsEnc *enc, int index, int64_t pts_us, bool force_keyframe)
{
  struct v4l2_plane plane;
  struct v4l2_buffer buf;
  bool ok;

  if (force_keyframe)
    set_ctrl (enc, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, true);

  memset (&buf, 0, sizeof buf);
  memset (&plane, 0, sizeof plane);
  buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  buf.memory = V4L2_MEMORY_DMABUF;
  buf.index = index;
  buf.m.planes = &plane;
  buf.length = 1;
  plane.m.fd = enc->inputs[index].dmabuf_fd;
  buf.timestamp.tv_sec = pts_us / 1000000;
  buf.timestamp.tv_usec = pts_us % 1000000;
  plane.bytesused = enc->inputs[index].size;
  plane.length = enc->inputs[index].size;

  pthread_mutex_lock (&enc->lock);
  ok = xioctl (enc->fd, VIDIOC_QBUF, &buf) == 0;
  if (ok)
    {
      enc->input_busy[index] = true;
      enc->n_busy++;
    }
  pthread_mutex_unlock (&enc->lock);

  if (!ok)
    fprintf (stderr, "xs-enc: QBUF(input %d): %s\n", index, strerror (errno));
  return ok;
}

int
xs_enc_poll_output (XsEnc           *enc,
                    int              timeout_ms,
                    XsEncOutputFunc  func,
                    void            *user_data)
{
  struct pollfd pfd = { .fd = enc->fd, .events = POLLIN };
  int delivered = 0;

  if (poll (&pfd, 1, timeout_ms) < 0)
    return errno == EINTR ? 0 : -1;
  if (pfd.revents & POLLERR)
    return -1;

  for (;;)
    {
      struct v4l2_plane plane;
      struct v4l2_buffer buf;

      memset (&buf, 0, sizeof buf);
      memset (&plane, 0, sizeof plane);
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buf.memory = V4L2_MEMORY_DMABUF;
      buf.m.planes = &plane;
      buf.length = 1;
      if (xioctl (enc->fd, VIDIOC_DQBUF, &buf) < 0)
        {
          if (errno == EAGAIN)
            break;
          fprintf (stderr, "xs-enc: DQBUF(output): %s\n", strerror (errno));
          return -1;
        }

      if (plane.bytesused > plane.data_offset)
        {
          XsEncOutputBuffer *out = &enc->outputs[buf.index];
          struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };

          xioctl (out->fd, DMA_BUF_IOCTL_SYNC, &sync);
          func ((const uint8_t *) out->data + plane.data_offset,
                plane.bytesused - plane.data_offset,
                (int64_t) buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec,
                (buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0,
                user_data);
          sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
          xioctl (out->fd, DMA_BUF_IOCTL_SYNC, &sync);
          delivered++;
        }

      if (!queue_output (enc, buf.index))
        {
          fprintf (stderr, "xs-enc: re-QBUF(output): %s\n", strerror (errno));
          return -1;
        }
    }

  pthread_mutex_lock (&enc->lock);
  reclaim_inputs (enc);
  pthread_mutex_unlock (&enc->lock);

  return delivered;
}
