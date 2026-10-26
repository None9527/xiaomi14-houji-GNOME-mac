#include "xs-gpu.h"

#define EGL_EGLEXT_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XS_GPU_MAX_SOURCES 16
#define XS_GPU_MAX_TARGETS 8

/* Each invocation converts a 4x2 pixel block; a work group covers 64x16. */
#define LOCAL_SIZE_X 16
#define LOCAL_SIZE_Y 8
#define STR_(x) #x
#define STR(x) STR_ (x)

#ifndef GL_EXT_EGL_image_storage
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC) (GLenum target,
                                                                  GLeglImageOES image,
                                                                  const GLint *attrib_list);
#endif

typedef struct
{
  bool used;
  int fd;
  uint32_t offset;
  uint32_t stride;
  int width;
  int height;
  EGLImageKHR image;
  GLuint texture;
} SourceEntry;

typedef struct
{
  bool used;
  int fd;
  int width;
  int height;
  EGLImageKHR y_image;
  EGLImageKHR uv_image;
  GLuint textures[2];
} TargetEntry;

struct _XsGpu
{
  EGLDisplay display;
  EGLContext context;

  PFNEGLCREATEIMAGEKHRPROC create_image;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
  PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC image_target_tex_storage;
  PFNEGLCREATESYNCKHRPROC create_sync;
  PFNEGLDESTROYSYNCKHRPROC destroy_sync;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;

  bool profiling;
  GLuint query;
  PFNGLGENQUERIESEXTPROC gen_queries;
  PFNGLDELETEQUERIESEXTPROC delete_queries;
  PFNGLBEGINQUERYEXTPROC begin_query;
  PFNGLENDQUERYEXTPROC end_query;
  PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_u64;

  GLuint program;
  uint64_t cpu_ns[4];

  SourceEntry sources[XS_GPU_MAX_SOURCES];
  TargetEntry targets[XS_GPU_MAX_TARGETS];
};

/*
 * One pass, BT.709 limited range. The NV12 planes are bound as RGBA8 images
 * a quarter of the pixel width wide (GLES has no R8 image format): a texel
 * of the luma image is four luma samples, a texel of the chroma image is
 * two CbCr pairs. Each source pixel is read exactly once.
 */
static const char *compute_shader =
  "#version 310 es\n"
  "layout (local_size_x = " STR (LOCAL_SIZE_X) ", local_size_y = " STR (LOCAL_SIZE_Y) ") in;\n"
  "layout (binding = 0) uniform highp sampler2D src;\n"
  "layout (rgba8, binding = 0) writeonly uniform highp image2D y_plane;\n"
  "layout (rgba8, binding = 1) writeonly uniform highp image2D uv_plane;\n"
  "layout (location = 0) uniform ivec4 blocks;  /* x0, y0, x1, y1 in 4x2 blocks */\n"
  "const vec3 K = vec3 (0.2126, 0.7152, 0.0722);\n"
  "float luma (vec3 c) { return 16.0 / 255.0 + dot (c, K) * (219.0 / 255.0); }\n"
  "vec2 chroma (vec3 c) {\n"
  "  float y = dot (c, K);\n"
  "  return vec2 (128.0 / 255.0 + (c.b - y) * (224.0 / 255.0 / 1.8556),\n"
  "               128.0 / 255.0 + (c.r - y) * (224.0 / 255.0 / 1.5748));\n"
  "}\n"
  "void main () {\n"
  "  ivec2 g = ivec2 (gl_GlobalInvocationID.xy) + blocks.xy;\n"
  "  if (any (greaterThanEqual (g, min (blocks.zw, imageSize (uv_plane)))))\n"
  "    return;\n"
  "  ivec2 p = ivec2 (g.x * 4, g.y * 2);\n"
  "  vec3 a0 = texelFetch (src, p, 0).rgb;\n"
  "  vec3 a1 = texelFetch (src, p + ivec2 (1, 0), 0).rgb;\n"
  "  vec3 a2 = texelFetch (src, p + ivec2 (2, 0), 0).rgb;\n"
  "  vec3 a3 = texelFetch (src, p + ivec2 (3, 0), 0).rgb;\n"
  "  vec3 b0 = texelFetch (src, p + ivec2 (0, 1), 0).rgb;\n"
  "  vec3 b1 = texelFetch (src, p + ivec2 (1, 1), 0).rgb;\n"
  "  vec3 b2 = texelFetch (src, p + ivec2 (2, 1), 0).rgb;\n"
  "  vec3 b3 = texelFetch (src, p + ivec2 (3, 1), 0).rgb;\n"
  "  imageStore (y_plane, ivec2 (g.x, p.y),\n"
  "              vec4 (luma (a0), luma (a1), luma (a2), luma (a3)));\n"
  "  imageStore (y_plane, ivec2 (g.x, p.y + 1),\n"
  "              vec4 (luma (b0), luma (b1), luma (b2), luma (b3)));\n"
  "  imageStore (uv_plane, g,\n"
  "              vec4 (chroma ((a0 + a1 + b0 + b1) * 0.25),\n"
  "                    chroma ((a2 + a3 + b2 + b3) * 0.25)));\n"
  "}\n";

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

static GLuint
compile_program (char *error, size_t error_size)
{
  GLuint shader = glCreateShader (GL_COMPUTE_SHADER);
  GLuint program;
  GLint ok;

  glShaderSource (shader, 1, &compute_shader, NULL);
  glCompileShader (shader);
  glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
    {
      char log[1024];

      glGetShaderInfoLog (shader, sizeof log, NULL, log);
      set_error (error, error_size, "compute shader: %s", log);
      glDeleteShader (shader);
      return 0;
    }

  program = glCreateProgram ();
  glAttachShader (program, shader);
  glLinkProgram (program);
  glDeleteShader (shader);
  glGetProgramiv (program, GL_LINK_STATUS, &ok);
  if (!ok)
    {
      set_error (error, error_size, "compute program link failed");
      glDeleteProgram (program);
      return 0;
    }
  return program;
}

XsGpu *
xs_gpu_new (char *error, size_t error_size)
{
  EGLint context_attribs[] = {
    EGL_CONTEXT_MAJOR_VERSION, 3,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_NONE, EGL_NONE,
    EGL_NONE,
  };
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
  XsGpu *gpu = calloc (1, sizeof *gpu);
  const char *extensions;

  get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
    eglGetProcAddress ("eglGetPlatformDisplayEXT");
  if (!get_platform_display)
    {
      set_error (error, error_size, "eglGetPlatformDisplayEXT missing");
      goto fail;
    }
  gpu->display = get_platform_display (EGL_PLATFORM_SURFACELESS_MESA,
                                       EGL_DEFAULT_DISPLAY, NULL);
  if (gpu->display == EGL_NO_DISPLAY ||
      !eglInitialize (gpu->display, NULL, NULL))
    {
      set_error (error, error_size, "EGL surfaceless display init failed");
      goto fail;
    }

  extensions = eglQueryString (gpu->display, EGL_EXTENSIONS);
  if (!strstr (extensions, "EGL_EXT_image_dma_buf_import") ||
      !strstr (extensions, "EGL_ANDROID_native_fence_sync") ||
      !strstr (extensions, "EGL_KHR_surfaceless_context"))
    {
      set_error (error, error_size, "missing EGL dmabuf/fence extensions");
      goto fail;
    }

  /* High priority: the conversion is on the latency path and must not
   * queue behind the compositor's rendering. */
  if (strstr (extensions, "EGL_IMG_context_priority"))
    {
      context_attribs[4] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
      context_attribs[5] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    }

  eglBindAPI (EGL_OPENGL_ES_API);
  gpu->context = eglCreateContext (gpu->display, EGL_NO_CONFIG_KHR,
                                   EGL_NO_CONTEXT, context_attribs);
  if (gpu->context == EGL_NO_CONTEXT && context_attribs[4] != EGL_NONE)
    {
      context_attribs[4] = EGL_NONE;
      gpu->context = eglCreateContext (gpu->display, EGL_NO_CONFIG_KHR,
                                       EGL_NO_CONTEXT, context_attribs);
    }
  if (gpu->context == EGL_NO_CONTEXT ||
      !eglMakeCurrent (gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context))
    {
      set_error (error, error_size, "GLES 3.1 context creation failed");
      goto fail;
    }

  gpu->create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress ("eglCreateImageKHR");
  gpu->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress ("eglDestroyImageKHR");
  gpu->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
    eglGetProcAddress ("glEGLImageTargetTexture2DOES");
  if (strstr ((const char *) glGetString (GL_EXTENSIONS), "GL_EXT_EGL_image_storage"))
    gpu->image_target_tex_storage = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)
      eglGetProcAddress ("glEGLImageTargetTexStorageEXT");
  gpu->create_sync = (PFNEGLCREATESYNCKHRPROC) eglGetProcAddress ("eglCreateSyncKHR");
  gpu->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC) eglGetProcAddress ("eglDestroySyncKHR");
  gpu->dup_native_fence_fd = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
    eglGetProcAddress ("eglDupNativeFenceFDANDROID");
  if (!gpu->create_image || !gpu->image_target_texture || !gpu->create_sync ||
      !gpu->dup_native_fence_fd)
    {
      set_error (error, error_size, "missing EGL/GLES entry points");
      goto fail;
    }

  {
    EGLint priority = 0;

    eglQueryContext (gpu->display, gpu->context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &priority);
    if (priority != EGL_CONTEXT_PRIORITY_HIGH_IMG)
      fprintf (stderr, "xs-gpu: running without high context priority\n");
  }

  gpu->program = compile_program (error, error_size);
  if (!gpu->program)
    goto fail;

  return gpu;

fail:
  xs_gpu_free (gpu);
  return NULL;
}

void
xs_gpu_forget_sources (XsGpu *gpu)
{
  for (int i = 0; i < XS_GPU_MAX_SOURCES; i++)
    {
      SourceEntry *s = &gpu->sources[i];

      if (!s->used)
        continue;
      glDeleteTextures (1, &s->texture);
      gpu->destroy_image (gpu->display, s->image);
      memset (s, 0, sizeof *s);
    }
}

void
xs_gpu_forget_targets (XsGpu *gpu)
{
  for (int i = 0; i < XS_GPU_MAX_TARGETS; i++)
    {
      TargetEntry *t = &gpu->targets[i];

      if (!t->used)
        continue;
      glDeleteTextures (2, t->textures);
      if (t->y_image != EGL_NO_IMAGE_KHR)
        gpu->destroy_image (gpu->display, t->y_image);
      if (t->uv_image != EGL_NO_IMAGE_KHR)
        gpu->destroy_image (gpu->display, t->uv_image);
      memset (t, 0, sizeof *t);
    }
}

void
xs_gpu_free (XsGpu *gpu)
{
  if (!gpu)
    return;

  if (gpu->context && gpu->context != EGL_NO_CONTEXT)
    {
      xs_gpu_forget_sources (gpu);
      xs_gpu_forget_targets (gpu);
      if (gpu->program)
        glDeleteProgram (gpu->program);
      if (gpu->query)
        gpu->delete_queries (1, &gpu->query);
      eglMakeCurrent (gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext (gpu->display, gpu->context);
    }
  if (gpu->display && gpu->display != EGL_NO_DISPLAY)
    eglTerminate (gpu->display);
  free (gpu);
}

void
xs_gpu_set_profiling (XsGpu *gpu, bool enabled)
{
  if (enabled && !gpu->query)
    {
      gpu->gen_queries = (PFNGLGENQUERIESEXTPROC) eglGetProcAddress ("glGenQueriesEXT");
      gpu->delete_queries = (PFNGLDELETEQUERIESEXTPROC) eglGetProcAddress ("glDeleteQueriesEXT");
      gpu->begin_query = (PFNGLBEGINQUERYEXTPROC) eglGetProcAddress ("glBeginQueryEXT");
      gpu->end_query = (PFNGLENDQUERYEXTPROC) eglGetProcAddress ("glEndQueryEXT");
      gpu->get_query_u64 = (PFNGLGETQUERYOBJECTUI64VEXTPROC)
        eglGetProcAddress ("glGetQueryObjectui64vEXT");
      if (!gpu->gen_queries || !gpu->delete_queries || !gpu->begin_query ||
          !gpu->end_query || !gpu->get_query_u64)
        return;
      gpu->gen_queries (1, &gpu->query);
    }
  gpu->profiling = enabled && gpu->query;
}

bool
xs_gpu_read_timings (XsGpu *gpu, XsGpuTimings *timings)
{
  GLuint64 ns = 0;

  if (!gpu->profiling)
    return false;
  gpu->get_query_u64 (gpu->query, GL_QUERY_RESULT_EXT, &ns);
  timings->convert_ns = ns;
  timings->cpu_setup_ns = gpu->cpu_ns[0];
  timings->cpu_dispatch_ns = gpu->cpu_ns[1];
  timings->cpu_flush_ns = gpu->cpu_ns[2];
  timings->cpu_fence_ns = gpu->cpu_ns[3];
  return true;
}

static uint64_t
now_ns (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static EGLImageKHR
import_plane (XsGpu    *gpu,
              int       fd,
              uint32_t  drm_format,
              uint64_t  modifier,
              int       width,
              int       height,
              uint32_t  offset,
              uint32_t  stride)
{
  EGLint attribs[] = {
    EGL_WIDTH, width,
    EGL_HEIGHT, height,
    EGL_LINUX_DRM_FOURCC_EXT, (EGLint) drm_format,
    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint) offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) stride,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint) (modifier & 0xffffffff),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint) (modifier >> 32),
    EGL_NONE,
  };

  /* Implicit modifiers are expressed by leaving the modifier out. */
  if (modifier == DRM_FORMAT_MOD_INVALID)
    attribs[12] = EGL_NONE;

  return gpu->create_image (gpu->display, EGL_NO_CONTEXT,
                            EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
}

static void
bind_image_to_texture (XsGpu *gpu, GLuint texture, EGLImageKHR image)
{
  glBindTexture (GL_TEXTURE_2D, texture);
  /* Immutable storage where available: required for image load/store by
   * the spec, and it spares the driver a completeness check per dispatch. */
  if (gpu->image_target_tex_storage)
    gpu->image_target_tex_storage (GL_TEXTURE_2D, image, NULL);
  else
    gpu->image_target_texture (GL_TEXTURE_2D, image);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static SourceEntry *
lookup_source (XsGpu *gpu, const XsGpuSource *source)
{
  SourceEntry *free_slot = NULL;

  for (int i = 0; i < XS_GPU_MAX_SOURCES; i++)
    {
      SourceEntry *s = &gpu->sources[i];

      if (!s->used)
        {
          if (!free_slot)
            free_slot = s;
          continue;
        }
      if (s->fd == source->fd && s->offset == source->offset &&
          s->stride == source->stride && s->width == source->width &&
          s->height == source->height)
        return s;
    }

  if (!free_slot)
    {
      xs_gpu_forget_sources (gpu);
      free_slot = &gpu->sources[0];
    }

  free_slot->image = import_plane (gpu, source->fd, source->drm_format,
                                   source->modifier, source->width,
                                   source->height, source->offset,
                                   source->stride);
  if (free_slot->image == EGL_NO_IMAGE_KHR)
    {
      fprintf (stderr, "xs-gpu: source import failed (0x%x)\n", eglGetError ());
      return NULL;
    }

  glGenTextures (1, &free_slot->texture);
  bind_image_to_texture (gpu, free_slot->texture, free_slot->image);

  free_slot->used = true;
  free_slot->fd = source->fd;
  free_slot->offset = source->offset;
  free_slot->stride = source->stride;
  free_slot->width = source->width;
  free_slot->height = source->height;
  return free_slot;
}

static TargetEntry *
lookup_target (XsGpu *gpu, const XsGpuTarget *target)
{
  TargetEntry *t = NULL;

  for (int i = 0; i < XS_GPU_MAX_TARGETS; i++)
    {
      TargetEntry *e = &gpu->targets[i];

      if (e->used && e->fd == target->fd &&
          e->width == target->width && e->height == target->height)
        return e;
    }

  for (int i = 0; i < XS_GPU_MAX_TARGETS && !t; i++)
    if (!gpu->targets[i].used)
      t = &gpu->targets[i];
  if (!t)
    {
      xs_gpu_forget_targets (gpu);
      t = &gpu->targets[0];
    }

  /* Four 8-bit samples per RGBA8 texel, in memory order. */
  t->y_image = import_plane (gpu, target->fd, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR,
                             target->width / 4, target->height, 0, target->stride);
  t->uv_image = import_plane (gpu, target->fd, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR,
                              target->width / 4, target->height / 2,
                              target->uv_offset, target->stride);
  t->used = true;
  t->fd = target->fd;
  t->width = target->width;
  t->height = target->height;
  if (t->y_image == EGL_NO_IMAGE_KHR || t->uv_image == EGL_NO_IMAGE_KHR)
    {
      fprintf (stderr, "xs-gpu: NV12 target import failed (0x%x)\n", eglGetError ());
      xs_gpu_forget_targets (gpu);
      return NULL;
    }

  glGenTextures (2, t->textures);
  bind_image_to_texture (gpu, t->textures[0], t->y_image);
  bind_image_to_texture (gpu, t->textures[1], t->uv_image);
  return t;
}

/* Dispatches the conversion of one rectangle, widened to whole 4x2 blocks. */
static void
dispatch_rect (const XsRect *rect)
{
  int x0 = rect->x / 4;
  int y0 = rect->y / 2;
  int x1 = (rect->x + rect->width + 3) / 4;
  int y1 = (rect->y + rect->height + 1) / 2;

  glUniform4i (0, x0, y0, x1, y1);
  glDispatchCompute ((x1 - x0 + LOCAL_SIZE_X - 1) / LOCAL_SIZE_X,
                     (y1 - y0 + LOCAL_SIZE_Y - 1) / LOCAL_SIZE_Y, 1);
}

int
xs_gpu_convert (XsGpu             *gpu,
                const XsGpuSource *source,
                const XsGpuTarget *target,
                const XsRegion    *region)
{
  static const EGLint sync_attribs[] = {
    EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
    EGL_NONE,
  };
  SourceEntry *src;
  TargetEntry *dst;
  EGLSyncKHR sync;
  int fence_fd;
  uint64_t t0 = now_ns (), t1, t2, t3;

  if (target->width % 4 || target->height % 2)
    {
      fprintf (stderr, "xs-gpu: %dx%d is not a multiple of 4x2\n",
               target->width, target->height);
      return -2;
    }
  if (region && xs_region_is_empty (region))
    return -1;

  src = lookup_source (gpu, source);
  dst = lookup_target (gpu, target);
  if (!src || !dst)
    return -2;

  glUseProgram (gpu->program);
  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, src->texture);
  glBindImageTexture (0, dst->textures[0], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
  glBindImageTexture (1, dst->textures[1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

  t1 = now_ns ();
  if (gpu->profiling)
    gpu->begin_query (GL_TIME_ELAPSED_EXT, gpu->query);
  if (!region || region->full)
    dispatch_rect (&(XsRect) { 0, 0, target->width, target->height });
  else
    for (int i = 0; i < region->n_rects; i++)
      dispatch_rect (&region->rects[i]);
  if (gpu->profiling)
    gpu->end_query (GL_TIME_ELAPSED_EXT);

  t2 = now_ns ();
  sync = gpu->create_sync (gpu->display, EGL_SYNC_NATIVE_FENCE_ANDROID, sync_attribs);
  glFlush ();
  t3 = now_ns ();
  gpu->cpu_ns[0] = t1 - t0;
  gpu->cpu_ns[1] = t2 - t1;
  gpu->cpu_ns[2] = t3 - t2;
  if (sync == EGL_NO_SYNC_KHR)
    {
      glFinish ();
      return -1;
    }
  fence_fd = gpu->dup_native_fence_fd (gpu->display, sync);
  gpu->destroy_sync (gpu->display, sync);
  gpu->cpu_ns[3] = now_ns () - t3;
  if (fence_fd < 0)
    {
      glFinish ();
      return -1;
    }
  return fence_fd;
}
