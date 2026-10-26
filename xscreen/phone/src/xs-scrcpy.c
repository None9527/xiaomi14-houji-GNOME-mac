#include "xs-scrcpy.h"

#include <glib.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- byte order ------------------------------------------------------- */

static void
put_be32 (uint8_t *p, uint32_t v)
{
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void
put_be64 (uint8_t *p, uint64_t v)
{
  put_be32 (p, v >> 32);
  put_be32 (p + 4, (uint32_t) v);
}

static uint16_t
be16 (const uint8_t *p)
{
  return (uint16_t) (p[0] << 8 | p[1]);
}

static uint32_t
be32 (const uint8_t *p)
{
  return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint64_t
be64 (const uint8_t *p)
{
  return (uint64_t) be32 (p) << 32 | be32 (p + 4);
}

/* --- video ------------------------------------------------------------ */

#define FLAG_CONFIG (UINT64_C (1) << 62)
#define FLAG_KEYFRAME (UINT64_C (1) << 61)

void
xs_scrcpy_session_header (uint8_t out[XS_SCRCPY_HEADER_SIZE], int width, int height)
{
  /* MSB set: a session packet; bit 0 of byte 3 would mean "resized by the
   * client", which this server never does on its own. */
  put_be32 (out, UINT32_C (0x80000000));
  put_be32 (out + 4, width);
  put_be32 (out + 8, height);
}

void
xs_scrcpy_packet_header (uint8_t out[XS_SCRCPY_HEADER_SIZE], int64_t pts_us,
                         bool config, bool keyframe, uint32_t size)
{
  uint64_t pts_flags = config ? FLAG_CONFIG : ((uint64_t) pts_us & (FLAG_KEYFRAME - 1));

  if (keyframe)
    pts_flags |= FLAG_KEYFRAME;
  put_be64 (out, pts_flags);
  put_be32 (out + 8, size);
}

size_t
xs_scrcpy_parameter_sets_length (const uint8_t *au, size_t size)
{
  size_t pos = 0, end = 0;

  while (pos + 4 <= size && au[pos] == 0 && au[pos + 1] == 0)
    {
      size_t start_code = au[pos + 2] == 1 ? 3 : (au[pos + 2] == 0 && au[pos + 3] == 1) ? 4 : 0;
      size_t nal = pos + start_code;
      uint8_t type;

      if (!start_code || nal >= size)
        break;
      type = (au[nal] >> 1) & 0x3f;
      if (type < 32 || type > 34)       /* VPS, SPS, PPS */
        break;
      for (end = nal; end + 3 <= size; end++)
        if (au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 ||
                                                  (end + 3 < size && au[end + 2] == 0 &&
                                                   au[end + 3] == 1)))
          break;
      if (end + 3 > size)
        end = size;
      pos = end;
    }
  return end;
}

/* --- device messages -------------------------------------------------- */

size_t
xs_scrcpy_clipboard_message (uint8_t *out, size_t out_size, const char *text)
{
  size_t len = strlen (text);

  if (out_size < 5 + len)
    return 0;
  out[0] = 0;                           /* DEVICE_MSG_TYPE_CLIPBOARD */
  put_be32 (out + 1, len);
  memcpy (out + 5, text, len);
  return 5 + len;
}

size_t
xs_scrcpy_ack_clipboard_message (uint8_t out[9], uint64_t sequence)
{
  out[0] = 1;                           /* DEVICE_MSG_TYPE_ACK_CLIPBOARD */
  put_be64 (out + 1, sequence);
  return 9;
}

/* --- key tables ------------------------------------------------------- */

/* Android AKEYCODE_* -> evdev. HOME opens the overview (Super), BACK is
 * Escape. */
static const uint16_t android_keys[] = {
  [3] = KEY_LEFTMETA, [4] = KEY_ESC,
  [7] = KEY_0, [8] = KEY_1, [9] = KEY_2, [10] = KEY_3, [11] = KEY_4,
  [12] = KEY_5, [13] = KEY_6, [14] = KEY_7, [15] = KEY_8, [16] = KEY_9,
  [19] = KEY_UP, [20] = KEY_DOWN, [21] = KEY_LEFT, [22] = KEY_RIGHT, [23] = KEY_ENTER,
  [24] = KEY_VOLUMEUP, [25] = KEY_VOLUMEDOWN,
  [29] = KEY_A, [30] = KEY_B, [31] = KEY_C, [32] = KEY_D, [33] = KEY_E, [34] = KEY_F,
  [35] = KEY_G, [36] = KEY_H, [37] = KEY_I, [38] = KEY_J, [39] = KEY_K, [40] = KEY_L,
  [41] = KEY_M, [42] = KEY_N, [43] = KEY_O, [44] = KEY_P, [45] = KEY_Q, [46] = KEY_R,
  [47] = KEY_S, [48] = KEY_T, [49] = KEY_U, [50] = KEY_V, [51] = KEY_W, [52] = KEY_X,
  [53] = KEY_Y, [54] = KEY_Z,
  [55] = KEY_COMMA, [56] = KEY_DOT, [57] = KEY_LEFTALT, [58] = KEY_RIGHTALT,
  [59] = KEY_LEFTSHIFT, [60] = KEY_RIGHTSHIFT, [61] = KEY_TAB, [62] = KEY_SPACE,
  [66] = KEY_ENTER, [67] = KEY_BACKSPACE, [68] = KEY_GRAVE, [69] = KEY_MINUS,
  [70] = KEY_EQUAL, [71] = KEY_LEFTBRACE, [72] = KEY_RIGHTBRACE, [73] = KEY_BACKSLASH,
  [74] = KEY_SEMICOLON, [75] = KEY_APOSTROPHE, [76] = KEY_SLASH,
  [82] = KEY_COMPOSE, [84] = KEY_SEARCH,
  [85] = KEY_PLAYPAUSE, [86] = KEY_STOPCD, [87] = KEY_NEXTSONG, [88] = KEY_PREVIOUSSONG,
  [89] = KEY_REWIND, [90] = KEY_FASTFORWARD, [91] = KEY_MICMUTE,
  [92] = KEY_PAGEUP, [93] = KEY_PAGEDOWN,
  [111] = KEY_ESC, [112] = KEY_DELETE, [113] = KEY_LEFTCTRL, [114] = KEY_RIGHTCTRL,
  [115] = KEY_CAPSLOCK, [116] = KEY_SCROLLLOCK, [117] = KEY_LEFTMETA, [118] = KEY_RIGHTMETA,
  [120] = KEY_SYSRQ, [121] = KEY_PAUSE, [122] = KEY_HOME, [123] = KEY_END,
  [124] = KEY_INSERT, [125] = KEY_FORWARD, [126] = KEY_PLAYCD, [127] = KEY_PAUSECD,
  [131] = KEY_F1, [132] = KEY_F2, [133] = KEY_F3, [134] = KEY_F4, [135] = KEY_F5,
  [136] = KEY_F6, [137] = KEY_F7, [138] = KEY_F8, [139] = KEY_F9, [140] = KEY_F10,
  [141] = KEY_F11, [142] = KEY_F12, [143] = KEY_NUMLOCK,
  [144] = KEY_KP0, [145] = KEY_KP1, [146] = KEY_KP2, [147] = KEY_KP3, [148] = KEY_KP4,
  [149] = KEY_KP5, [150] = KEY_KP6, [151] = KEY_KP7, [152] = KEY_KP8, [153] = KEY_KP9,
  [154] = KEY_KPSLASH, [155] = KEY_KPASTERISK, [156] = KEY_KPMINUS, [157] = KEY_KPPLUS,
  [158] = KEY_KPDOT, [159] = KEY_KPCOMMA, [160] = KEY_KPENTER, [161] = KEY_KPEQUAL,
  [164] = KEY_MUTE, [168] = KEY_ZOOMIN, [169] = KEY_ZOOMOUT,
  [220] = KEY_BRIGHTNESSDOWN, [221] = KEY_BRIGHTNESSUP,
};

/* USB HID keyboard usages 0x00-0x65 -> evdev (as Linux's hid-input). */
static const uint16_t hid_keys[0x66] = {
  0, 0, 0, 0, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L,
  KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y,
  KEY_Z, KEY_1, KEY_2,
  KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_ENTER, KEY_ESC, KEY_BACKSPACE,
  KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL, KEY_LEFTBRACE,
  KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
  KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
  KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE,
  KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN, KEY_RIGHT,
  KEY_LEFT, KEY_DOWN, KEY_UP, KEY_NUMLOCK, KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS,
  KEY_KPPLUS, KEY_KPENTER, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7,
  KEY_KP8, KEY_KP9, KEY_KP0, KEY_KPDOT, KEY_102ND, KEY_COMPOSE,
};

/* HID modifier bits: left ctrl, shift, alt, gui; right ctrl, shift, alt, gui. */
static const uint16_t hid_modifiers[8] = {
  KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
  KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
};

/* Printable ASCII on a US layout: key, and whether Shift is needed. */
static bool
ascii_key (char c, uint16_t *key, bool *shift)
{
  static const char unshifted[] = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./ ";
  static const char shifted[] = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
  static const uint16_t keys[] = {
    KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O,
    KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_A, KEY_S, KEY_D, KEY_F,
    KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_Z, KEY_X,
    KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SPACE,
  };
  const char *p;

  if (c == '\0')
    return false;
  if ((p = strchr (unshifted, c)))
    {
      *key = keys[p - unshifted];
      *shift = false;
      return true;
    }
  if ((p = strchr (shifted, c)))
    {
      *key = keys[p - shifted];
      *shift = true;
      return true;
    }
  if (c == '\n')
    {
      *key = KEY_ENTER;
      *shift = false;
      return true;
    }
  if (c == '\t')
    {
      *key = KEY_TAB;
      *shift = false;
      return true;
    }
  return false;
}

/* --- control ---------------------------------------------------------- */

#define POINTER_ID_MOUSE UINT64_MAX     /* SC_POINTER_ID_MOUSE (-1) */
/* Stream pixels per smooth (trackpad) scroll notch. */
#define SCROLL_PIXELS_PER_NOTCH 60.0f
#define UHID_KEYBOARD_INPUT_SIZE 8
#define UHID_MOUSE_INPUT_SIZE 5

/* Android MotionEvent button bits -> evdev, and HID mouse button bits. */
static const uint16_t pointer_buttons[5] = {
  BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
};

struct _XsScrcpyControl
{
  XsScrcpyCallbacks callbacks;
  int width;
  int height;
  uint32_t mouse_buttons;               /* pressed buttons (Android bits) */
  bool finger_down;                     /* touch acting as the left button */
  uint8_t hid_keyboard[UHID_KEYBOARD_INPUT_SIZE];
  uint8_t hid_mouse_buttons;
  bool debug;                           /* XS_DEBUG: log messages and events */
};

XsScrcpyControl *
xs_scrcpy_control_new (const XsScrcpyCallbacks *callbacks, int width, int height)
{
  XsScrcpyControl *control = g_new0 (XsScrcpyControl, 1);

  control->callbacks = *callbacks;
  control->debug = g_getenv ("XS_DEBUG") != NULL;
  control->width = width;
  control->height = height;
  return control;
}

void
xs_scrcpy_control_free (XsScrcpyControl *control)
{
  g_free (control);
}

void
xs_scrcpy_control_set_size (XsScrcpyControl *control, int width, int height)
{
  control->width = width;
  control->height = height;
}

static void
emit (XsScrcpyControl *control, XsInputEvent event)
{
  if (control->debug)
    fprintf (stderr, "xs-scrcpy:   event type %u code %u value %d (%.1f, %.1f)\n",
             event.type, event.code, event.value, event.x, event.y);
  control->callbacks.input (&event, control->callbacks.user_data);
}

static void
emit_key (XsScrcpyControl *control, uint16_t key, bool down)
{
  emit (control, (XsInputEvent) { .type = XS_INPUT_KEY, .code = key, .value = down });
}

static void
tap_keys (XsScrcpyControl *control, uint16_t modifier, uint16_t key)
{
  if (modifier)
    emit_key (control, modifier, true);
  emit_key (control, key, true);
  emit_key (control, key, false);
  if (modifier)
    emit_key (control, modifier, false);
}

/* A position in the client's frame size, mapped to the current stream. */
static void
move_to (XsScrcpyControl *control, const uint8_t *position)
{
  float x = (int32_t) be32 (position), y = (int32_t) be32 (position + 4);
  uint16_t w = be16 (position + 8), h = be16 (position + 10);

  if (w && h && (w != control->width || h != control->height))
    {
      x = x * control->width / w;
      y = y * control->height / h;
    }
  emit (control, (XsInputEvent) { .type = XS_INPUT_POINTER_ABSOLUTE, .x = x, .y = y });
}

static void
set_buttons (XsScrcpyControl *control, uint32_t buttons)
{
  for (int i = 0; i < 5; i++)
    {
      uint32_t bit = 1u << i;

      if ((buttons & bit) != (control->mouse_buttons & bit))
        emit (control, (XsInputEvent) { .type = XS_INPUT_BUTTON, .code = pointer_buttons[i],
                                        .value = (buttons & bit) != 0 });
    }
  control->mouse_buttons = buttons;
}

static void
handle_keycode (XsScrcpyControl *control, const uint8_t *m)
{
  int action = m[1];
  uint32_t keycode = be32 (m + 2), repeat = be32 (m + 6);
  uint16_t key = keycode < G_N_ELEMENTS (android_keys) ? android_keys[keycode] : 0;

  /* The compositor repeats held keys itself. */
  if (!key || action > 1 || (action == 0 && repeat > 0))
    return;
  emit_key (control, key, action == 0);
}

static void
handle_text (XsScrcpyControl *control, const char *text, size_t len)
{
  for (size_t i = 0; i < len; i++)
    {
      uint16_t key;
      bool shift;

      /* Text beyond US-layout ASCII (an IME commit) has no key to press;
       * --keyboard=uhid lets GNOME's own input method handle it. */
      if (ascii_key (text[i], &key, &shift))
        tap_keys (control, shift ? KEY_LEFTSHIFT : 0, key);
    }
}

static void
handle_touch (XsScrcpyControl *control, const uint8_t *m)
{
  int action = m[1];
  uint64_t pointer_id = be64 (m + 2);
  uint32_t buttons = be32 (m + 28);

  move_to (control, m + 10);
  if (pointer_id == POINTER_ID_MOUSE)
    {
      set_buttons (control, buttons);
      return;
    }
  /* A finger (or scrcpy's simulated one) acts as the left button. */
  if (action == 0 && !control->finger_down)           /* ACTION_DOWN */
    {
      control->finger_down = true;
      emit (control, (XsInputEvent) { .type = XS_INPUT_BUTTON, .code = BTN_LEFT, .value = 1 });
    }
  else if ((action == 1 || action == 3) && control->finger_down)   /* UP, CANCEL */
    {
      control->finger_down = false;
      emit (control, (XsInputEvent) { .type = XS_INPUT_BUTTON, .code = BTN_LEFT, .value = 0 });
    }
}

static void
scroll_axis (XsScrcpyControl *control, int16_t fixed, bool horizontal)
{
  /* Fixed point of notches / 16; positive means up (or right), while
   * libei scrolls down for positive values. */
  float notches = fixed / 2048.0f;
  float rounded = roundf (notches);

  if (notches == 0)
    return;
  if (fabsf (notches - rounded) < 0.01f)
    emit (control, (XsInputEvent) { .type = XS_INPUT_SCROLL_DISCRETE, .code = horizontal,
                                    .value = horizontal ? (int) rounded : -(int) rounded });
  else if (horizontal)
    emit (control, (XsInputEvent) { .type = XS_INPUT_SCROLL,
                                    .x = notches * SCROLL_PIXELS_PER_NOTCH });
  else
    emit (control, (XsInputEvent) { .type = XS_INPUT_SCROLL,
                                    .y = -notches * SCROLL_PIXELS_PER_NOTCH });
}

static void
handle_hid_keyboard (XsScrcpyControl *control, const uint8_t *report)
{
  uint8_t *previous = control->hid_keyboard;
  uint8_t changed = previous[0] ^ report[0];

  for (int i = 0; i < 8; i++)
    if (changed & (1 << i))
      emit_key (control, hid_modifiers[i], report[0] & (1 << i));

  /* Keys: releases first, then presses (bytes 2..7 hold usages). */
  for (int i = 2; i < UHID_KEYBOARD_INPUT_SIZE; i++)
    if (previous[i] && previous[i] < G_N_ELEMENTS (hid_keys) &&
        !memchr (report + 2, previous[i], UHID_KEYBOARD_INPUT_SIZE - 2))
      emit_key (control, hid_keys[previous[i]], false);
  for (int i = 2; i < UHID_KEYBOARD_INPUT_SIZE; i++)
    if (report[i] && report[i] < G_N_ELEMENTS (hid_keys) && hid_keys[report[i]] &&
        !memchr (previous + 2, report[i], UHID_KEYBOARD_INPUT_SIZE - 2))
      emit_key (control, hid_keys[report[i]], true);
  memcpy (previous, report, UHID_KEYBOARD_INPUT_SIZE);
}

static void
handle_hid_mouse (XsScrcpyControl *control, const uint8_t *report)
{
  int8_t dx = report[1], dy = report[2], wheel = report[3], hwheel = report[4];
  uint8_t changed = report[0] ^ control->hid_mouse_buttons;

  if (dx || dy)
    emit (control, (XsInputEvent) { .type = XS_INPUT_POINTER_RELATIVE, .x = dx, .y = dy });
  for (int i = 0; i < 5; i++)
    if (changed & (1 << i))
      emit (control, (XsInputEvent) { .type = XS_INPUT_BUTTON, .code = pointer_buttons[i],
                                      .value = (report[0] & (1 << i)) != 0 });
  control->hid_mouse_buttons = report[0];
  if (wheel)
    emit (control, (XsInputEvent) { .type = XS_INPUT_SCROLL_DISCRETE, .code = 0, .value = -wheel });
  if (hwheel)
    emit (control, (XsInputEvent) { .type = XS_INPUT_SCROLL_DISCRETE, .code = 1, .value = hwheel });
}

/* Size of the message at `m`: 0 while incomplete, -1 if unknown. */
static long
message_size (const uint8_t *m, size_t avail)
{
  size_t need;

#define NEED(n) do { need = (n); if (avail < need) return 0; } while (0)
  switch (m[0])
    {
    case 0:  return avail < 14 ? 0 : 14;               /* INJECT_KEYCODE */
    case 1:  NEED (5); NEED (5 + be32 (m + 1)); return need;   /* INJECT_TEXT */
    case 2:  return avail < 32 ? 0 : 32;               /* INJECT_TOUCH_EVENT */
    case 3:  return avail < 21 ? 0 : 21;               /* INJECT_SCROLL_EVENT */
    case 4: case 8: case 10: case 18:
             return avail < 2 ? 0 : 2;                 /* one-byte payloads */
    case 5: case 6: case 7: case 11: case 15: case 17: case 19: case 20:
             return 1;                                 /* no payload */
    case 9:  NEED (14); NEED (14 + be32 (m + 10)); return need;  /* SET_CLIPBOARD */
    case 12: NEED (8); NEED (8 + m[7] + 2); NEED (need + be16 (m + 8 + m[7])); return need;
    case 13: NEED (5); NEED (5 + be16 (m + 3)); return need;     /* UHID_INPUT */
    case 14: return avail < 3 ? 0 : 3;                 /* UHID_DESTROY */
    case 16: NEED (2); NEED (2 + m[1]); return need;   /* START_APP */
    case 21: return avail < 5 ? 0 : 5;                 /* RESIZE_DISPLAY */
    case 22: NEED (5); NEED (5 + be32 (m + 1)); return need;     /* SCAN_FILE */
    default: return -1;
    }
#undef NEED
}

static void
handle_message (XsScrcpyControl *control, const uint8_t *m, size_t size)
{
  const XsScrcpyCallbacks *cb = &control->callbacks;

  if (control->debug)
    fprintf (stderr, "xs-scrcpy: message %u (%zu bytes)%s\n", m[0], size,
             m[0] == 13 ? " uhid input" : "");

  switch (m[0])
    {
    case 0:
      handle_keycode (control, m);
      break;
    case 1:
      handle_text (control, (const char *) m + 5, size - 5);
      break;
    case 2:
      handle_touch (control, m);
      break;
    case 3:
      move_to (control, m + 1);
      scroll_axis (control, (int16_t) be16 (m + 13), true);
      scroll_axis (control, (int16_t) be16 (m + 15), false);
      break;
    case 4:                                            /* BACK_OR_SCREEN_ON */
      if (m[1] <= 1)
        emit_key (control, KEY_ESC, m[1] == 0);
      break;
    case 8:                                            /* GET_CLIPBOARD */
      if (cb->get_clipboard)
        cb->get_clipboard (m[1], cb->user_data);
      break;
    case 9:                                            /* SET_CLIPBOARD */
      if (cb->set_clipboard)
        {
          g_autofree char *text = g_strndup ((const char *) m + 14, size - 14);
          cb->set_clipboard (text, m[9] != 0, be64 (m + 1), cb->user_data);
        }
      break;
    case 13:                                           /* UHID_INPUT */
      {
        uint16_t len = be16 (m + 3);

        if (len == UHID_KEYBOARD_INPUT_SIZE)
          handle_hid_keyboard (control, m + 5);
        else if (len == UHID_MOUSE_INPUT_SIZE)
          handle_hid_mouse (control, m + 5);
      }
      break;
    case 14:                                           /* UHID_DESTROY: release all */
      {
        static const uint8_t released[UHID_KEYBOARD_INPUT_SIZE];
        static const uint8_t mouse_released[UHID_MOUSE_INPUT_SIZE];

        handle_hid_keyboard (control, released);
        handle_hid_mouse (control, mouse_released);
      }
      break;
    case 16:                                           /* START_APP */
      if (cb->start_app && m[1])
        {
          g_autofree char *name = g_strndup ((const char *) m + 2, m[1]);
          /* "+name" asks to force-stop first, "?label" to search by label;
           * desktop apps are matched by name either way. */
          cb->start_app (name[0] == '+' || name[0] == '?' ? name + 1 : name, cb->user_data);
        }
      break;
    case 17:                                           /* RESET_VIDEO */
      if (cb->keyframe)
        cb->keyframe (cb->user_data);
      break;
    default:
      /* Panels, rotation, display power, camera, resize (the client scales
       * the picture; the monitor's mode is chosen in GNOME Settings), UHID
       * creation and media scans have no desktop counterpart. */
      break;
    }
}

long
xs_scrcpy_control_feed (XsScrcpyControl *control, const uint8_t *data, size_t size)
{
  size_t offset = 0;

  while (offset < size)
    {
      long len = message_size (data + offset, size - offset);

      if (len < 0)
        return -1;
      if (len == 0)
        break;
      handle_message (control, data + offset, len);
      offset += len;
    }
  return offset;
}
