/* Checks xs-scrcpy's translation of scrcpy control messages into input
 * events, without a desktop. */
#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "../src/xs-scrcpy.h"

static XsInputEvent events[64];
static int n_events;

static void
record (const XsInputEvent *event, void *user_data)
{
  assert (n_events < 64);
  events[n_events++] = *event;
}

static int
feed (XsScrcpyControl *control, const uint8_t *data, size_t size)
{
  n_events = 0;
  assert (xs_scrcpy_control_feed (control, data, size) == (long) size);
  return n_events;
}

static void
expect_key (int i, int code, int value)
{
  if (events[i].type != XS_INPUT_KEY || (int) events[i].code != code || events[i].value != value)
    {
      fprintf (stderr, "event %d: type %u code %u value %d, expected key %d=%d\n", i,
               events[i].type, events[i].code, events[i].value, code, value);
      assert (0);
    }
}

int
main (void)
{
  XsScrcpyCallbacks callbacks = { .input = record };
  XsScrcpyControl *control = xs_scrcpy_control_new (&callbacks, 1600, 1000);

  /* INJECT_KEYCODE: ESCAPE down, a repeat (ignored), up. */
  {
    const uint8_t down[14] = { 0, 0, 0, 0, 0, 111 };
    const uint8_t repeat[14] = { 0, 0, 0, 0, 0, 111, 0, 0, 0, 1 };
    const uint8_t up[14] = { 0, 1, 0, 0, 0, 111 };
    assert (feed (control, down, 14) == 1);
    expect_key (0, KEY_ESC, 1);
    assert (feed (control, repeat, 14) == 0);
    assert (feed (control, up, 14) == 1);
    expect_key (0, KEY_ESC, 0);
  }

  /* INJECT_TEXT "Hi!": Shift+H, i, Shift+1. */
  {
    const uint8_t text[] = { 1, 0, 0, 0, 3, 'H', 'i', '!' };
    assert (feed (control, text, sizeof text) == 10);
    expect_key (0, KEY_LEFTSHIFT, 1);
    expect_key (1, KEY_H, 1);
    expect_key (2, KEY_H, 0);
    expect_key (3, KEY_LEFTSHIFT, 0);
    expect_key (4, KEY_I, 1);
    expect_key (6, KEY_LEFTSHIFT, 1);
    expect_key (7, KEY_1, 1);
  }

  /* UHID keyboard: Ctrl down + 'a', then all released. */
  {
    const uint8_t press[] = { 13, 0, 1, 0, 8, 0x01, 0, 0x04, 0, 0, 0, 0, 0 };
    const uint8_t release[] = { 13, 0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
    assert (feed (control, press, sizeof press) == 2);
    expect_key (0, KEY_LEFTCTRL, 1);
    expect_key (1, KEY_A, 1);
    assert (feed (control, release, sizeof release) == 2);
    expect_key (0, KEY_LEFTCTRL, 0);
    expect_key (1, KEY_A, 0);
  }

  /* UHID mouse: right button with motion, wheel up one notch. */
  {
    const uint8_t report[] = { 13, 0, 2, 0, 5, 0x02, 5, (uint8_t) -3, 1, 0 };
    assert (feed (control, report, sizeof report) == 3);
    assert (events[0].type == XS_INPUT_POINTER_RELATIVE && events[0].x == 5 && events[0].y == -3);
    assert (events[1].type == XS_INPUT_BUTTON && events[1].code == BTN_RIGHT && events[1].value == 1);
    assert (events[2].type == XS_INPUT_SCROLL_DISCRETE && events[2].code == 0 && events[2].value == -1);
  }

  /* Mouse click through INJECT_TOUCH_EVENT: middle button (Android 4). */
  {
    uint8_t touch[32] = { 2, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0, 0, 0x01, 0x90, 0, 0, 0x00, 0xc8, 0x06, 0x40, 0x03, 0xe8 };
    touch[31] = 4;              /* buttons: TERTIARY */
    assert (feed (control, touch, 32) == 2);
    assert (events[0].type == XS_INPUT_POINTER_ABSOLUTE && events[0].x == 400 && events[0].y == 200);
    assert (events[1].type == XS_INPUT_BUTTON && events[1].code == BTN_MIDDLE && events[1].value == 1);
  }

  /* INJECT_SCROLL: one notch up (vscroll = 1/16 in i16 fixed point). */
  {
    const uint8_t scroll[21] = { 3, 0, 0, 0, 10, 0, 0, 0, 10, 0x06, 0x40, 0x03, 0xe8,
                                 0, 0, 0x08, 0x00 };
    assert (feed (control, scroll, 21) == 2);
    assert (events[1].type == XS_INPUT_SCROLL_DISCRETE && events[1].code == 0 && events[1].value == -1);
  }

  /* Split delivery: a message completes across two reads. */
  {
    const uint8_t down[14] = { 0, 0, 0, 0, 0, 29 };
    n_events = 0;
    assert (xs_scrcpy_control_feed (control, down, 7) == 0);
    assert (xs_scrcpy_control_feed (control, down, 14) == 14);
    expect_key (0, KEY_A, 1);
  }

  /* An unknown type is a protocol error. */
  {
    const uint8_t bad[1] = { 200 };
    assert (xs_scrcpy_control_feed (control, bad, 1) == -1);
  }

  xs_scrcpy_control_free (control);
  printf ("test-scrcpy: all passed\n");
  return 0;
}
