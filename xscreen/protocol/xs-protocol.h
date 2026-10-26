/*
 * xscreen wire protocol (phone <-> Mac over an adb-forwarded TCP socket).
 *
 * Every message is a fixed header followed by `length` payload bytes.
 * All integers are little-endian. Both sides are little-endian ARM, so the
 * structs are sent as-is; they are packed and sized explicitly.
 */
#pragma once

#include <stdint.h>

#define XS_PROTOCOL_MAGIC 0x31435358u   /* "XSC1" */
#define XS_PROTOCOL_VERSION 1
#define XS_DEFAULT_PORT 7845
/* scrcpy streams, relayed by xscreen-scrcpy-server from the stock client.
 * Each connection starts with one line naming its role:
 *   "XSCRCPY1 <video|audio|control> <session> [size=WxH] [cursor]\n"
 * where "cursor" (scrcpy's --show-touches) draws the pointer into the video. */
#define XS_SCRCPY_PORT 7846

typedef enum
{
  /* phone -> Mac */
  XS_MSG_SERVER_HELLO = 1,   /* XsServerHello */
  XS_MSG_VIDEO_CONFIG = 2,   /* XsVideoConfig: stream (re)starts at this size */
  XS_MSG_VIDEO_FRAME = 3,    /* XsVideoFrame + Annex-B access unit */
  XS_MSG_CURSOR = 4,         /* XsCursor [+ RGBA bitmap] */
  XS_MSG_LAYOUT = 5,         /* XsLayout: edges leading to other monitors */
  XS_MSG_CLIPBOARD = 6,      /* XsClipboardHeader + content: GNOME's clipboard changed */
  XS_MSG_AUDIO = 7,          /* XsAudioFrame + PCM: desktop sound */
  XS_MSG_FILE_BEGIN = 8,     /* XsFileBegin + path: next file of a transfer */
  XS_MSG_FILE_DATA = 9,      /* XsFileData + bytes */
  XS_MSG_FILE_END = 10,      /* XsFileEnd: transfer complete */

  /* Mac -> phone */
  XS_MSG_CLIENT_HELLO = 64,  /* XsClientHello */
  XS_MSG_REQUEST_KEYFRAME = 65,
  XS_MSG_INPUT = 67,         /* XsInputEvent */
  XS_MSG_CLIENT_CLIPBOARD = 68,   /* XsClipboardHeader + content: the Mac's clipboard changed */
  XS_MSG_CLIENT_FILE_BEGIN = 72,  /* as XS_MSG_FILE_*, Mac to phone */
  XS_MSG_CLIENT_FILE_DATA = 73,
  XS_MSG_CLIENT_FILE_END = 74,
} XsMessageType;

typedef struct __attribute__ ((packed))
{
  uint32_t magic;
  uint16_t type;
  uint16_t flags;
  uint32_t length;           /* payload bytes after this header */
} XsMessageHeader;

typedef struct __attribute__ ((packed))
{
  uint32_t version;
  uint32_t codecs;           /* bit 0: HEVC, bit 1: H.264 */
} XsServerHello;

/* Describes the client's display. The phone offers GNOME a virtual
 * monitor with this display's identity and modes derived from its size;
 * resolution, scale and position are then chosen in GNOME Settings. */
typedef struct __attribute__ ((packed))
{
  uint32_t version;
  uint32_t width;            /* display size in pixels (full screen mode) */
  uint32_t height;
  uint32_t max_fps;
  uint32_t scale_percent;    /* the display's own HiDPI scale, 200 = 2x */
  uint32_t window_width;     /* window to show the picture in, pixels (the */
  uint32_t window_height;    /* preferred mode); 0: the whole display */
  char display_id[40];       /* stable id (UUID), NUL-terminated */
  char display_name[64];     /* human readable, NUL-terminated UTF-8 */
} XsClientHello;

typedef struct __attribute__ ((packed))
{
  uint32_t codec;            /* 1: HEVC, 2: H.264 */
  uint32_t width;
  uint32_t height;
} XsVideoConfig;

#define XS_FRAME_KEYFRAME (1u << 0)
#define XS_FRAME_REFINE   (1u << 1)  /* quality pass over a static picture */

typedef struct __attribute__ ((packed))
{
  int64_t pts_us;            /* capture time, phone CLOCK_MONOTONIC */
  uint32_t flags;
} XsVideoFrame;

typedef struct __attribute__ ((packed))
{
  int32_t x;                 /* hotspot position in stream pixels */
  int32_t y;
  int32_t hotspot_x;
  int32_t hotspot_y;
  uint32_t visible;
  uint32_t bitmap_width;     /* 0: sprite unchanged */
  uint32_t bitmap_height;    /* followed by width*height*4 RGBA bytes */
} XsCursor;


/* Edges of the virtual monitor that border another monitor: moving the
 * pointer across them leaves the client's picture. */
#define XS_EDGE_LEFT   (1u << 0)
#define XS_EDGE_RIGHT  (1u << 1)
#define XS_EDGE_TOP    (1u << 2)
#define XS_EDGE_BOTTOM (1u << 3)

typedef struct __attribute__ ((packed))
{
  uint32_t edges;
} XsLayout;

typedef enum
{
  XS_INPUT_POINTER_ABSOLUTE = 1, /* x, y in stream pixels (float) */
  XS_INPUT_POINTER_RELATIVE = 2, /* dx, dy in stream pixels (float) */
  XS_INPUT_BUTTON = 3,           /* code: evdev BTN_*, value: 1 down / 0 up */
  XS_INPUT_SCROLL = 4,           /* dx, dy smooth, stream pixels */
  XS_INPUT_SCROLL_DISCRETE = 5,  /* code: 0 vertical / 1 horizontal, value: steps */
  XS_INPUT_KEY = 6,              /* code: evdev KEY_*, value: 1 down / 0 up */
} XsInputType;

typedef struct __attribute__ ((packed))
{
  uint32_t type;
  uint32_t code;
  int32_t value;
  float x;
  float y;
} XsInputEvent;

/* Clipboard content. Files are not inlined: the sender follows up with a
 * file transfer (XS_FILE_FOR_CLIPBOARD) and the receiver puts the received
 * files on its clipboard once it ends. */
typedef enum
{
  XS_CLIPBOARD_TEXT = 1,         /* UTF-8 */
  XS_CLIPBOARD_IMAGE = 2,        /* PNG */
} XsClipboardFormat;

#define XS_CLIPBOARD_MAX (32u << 20)

typedef struct __attribute__ ((packed))
{
  uint32_t format;               /* XsClipboardFormat */
} XsClipboardHeader;

/* Desktop sound: interleaved signed 16-bit PCM. */
typedef struct __attribute__ ((packed))
{
  int64_t pts_us;                /* phone CLOCK_MONOTONIC */
  uint32_t rate;
  uint16_t channels;
  uint16_t reserved;
} XsAudioFrame;

/* File transfers, the same in both directions: per entry a BEGIN (then
 * `size` bytes of DATA for a regular file), then one END. Paths are
 * relative, '/'-separated UTF-8; receivers reject absolute paths and "..".
 * One transfer at a time per direction. */
#define XS_FILE_FOR_DOWNLOADS 0          /* save in the Downloads folder */
#define XS_FILE_FOR_CLIPBOARD 1          /* put on the clipboard when done */
#define XS_FILE_DIRECTORY (1u << 0)
#define XS_FILE_CHUNK (256u << 10)
#define XS_FILE_CLIPBOARD_MAX (512ull << 20)   /* larger copies are not sent */

typedef struct __attribute__ ((packed))
{
  uint32_t transfer;
  uint32_t purpose;              /* XS_FILE_FOR_* */
  uint32_t flags;                /* XS_FILE_DIRECTORY */
  uint32_t reserved;
  uint64_t size;
} XsFileBegin;                   /* + path */

typedef struct __attribute__ ((packed))
{
  uint32_t transfer;
} XsFileData;                    /* + bytes */

typedef struct __attribute__ ((packed))
{
  uint32_t transfer;
  uint32_t status;               /* 0: complete, else abandoned */
} XsFileEnd;
