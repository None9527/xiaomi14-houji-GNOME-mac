/* Unit tests for xs-damage. Build: cc -I../src test-damage.c ../src/xs-damage.c */
#include "xs-damage.h"

#include <assert.h>
#include <stdio.h>

#define W 2560
#define H 1600

static XsRegion
region_of (int x, int y, int w, int h)
{
  XsRegion r;
  XsRect rect = { x, y, w, h };

  xs_region_clear (&r);
  xs_region_add (&r, &rect, W, H);
  return r;
}

static void
test_region_basics (void)
{
  XsRegion r;
  XsRect off = { -10, -10, 20, 20 };
  XsRect outside = { W + 5, 0, 10, 10 };
  XsRect whole = { 0, 0, W, H };

  xs_region_clear (&r);
  assert (xs_region_is_empty (&r));

  xs_region_add (&r, &off, W, H);
  assert (r.n_rects == 1 && r.rects[0].x == 0 && r.rects[0].width == 10);

  xs_region_add (&r, &outside, W, H);
  assert (r.n_rects == 1);

  /* Contained rectangles are not stored twice. */
  xs_region_add (&r, &(XsRect) { 2, 2, 3, 3 }, W, H);
  assert (r.n_rects == 1);

  xs_region_add (&r, &whole, W, H);
  assert (r.full && xs_region_area (&r, W, H) == (int64_t) W * H);
}

static void
test_region_collapse (void)
{
  XsRegion r;

  xs_region_clear (&r);
  for (int i = 0; i <= XS_REGION_MAX_RECTS; i++)
    xs_region_add (&r, &(XsRect) { i * 100, i * 50, 10, 10 }, W, H);

  /* Over capacity: bounding box of everything, which still covers all. */
  assert (r.n_rects == 2);
  for (int i = 0; i <= XS_REGION_MAX_RECTS; i++)
    {
      XsRect probe = { i * 100, i * 50, 10, 10 };
      bool covered = false;

      for (int j = 0; j < r.n_rects; j++)
        {
          const XsRect *c = &r.rects[j];

          covered |= probe.x >= c->x && probe.y >= c->y &&
                     probe.x + probe.width <= c->x + c->width &&
                     probe.y + probe.height <= c->y + c->height;
        }
      assert (covered);
    }
}

static void
test_tracker (void)
{
  XsDamageTracker t;
  XsRegion repair;
  XsRegion a = region_of (0, 0, 100, 100);
  XsRegion b = region_of (500, 500, 50, 50);
  XsRegion empty;

  xs_region_clear (&empty);
  xs_damage_tracker_reset (&t, W, H);

  /* Unwritten slots always need everything. */
  xs_damage_tracker_add_frame (&t, &a);
  xs_damage_tracker_get_repair (&t, 0, &repair);
  assert (repair.full);
  xs_damage_tracker_mark_slot (&t, 0);

  /* A slot holding the newest frame needs nothing. */
  xs_damage_tracker_get_repair (&t, 0, &repair);
  assert (xs_region_is_empty (&repair));

  /* Two frames later, slot 0 needs both frames' damage. */
  xs_damage_tracker_add_frame (&t, &b);
  xs_damage_tracker_add_frame (&t, &a);
  xs_damage_tracker_get_repair (&t, 0, &repair);
  assert (!repair.full && xs_region_area (&repair, W, H) == 100 * 100 + 50 * 50);

  /* Frames without damage add nothing. */
  xs_damage_tracker_mark_slot (&t, 0);
  xs_damage_tracker_add_frame (&t, &empty);
  xs_damage_tracker_get_repair (&t, 0, &repair);
  assert (xs_region_is_empty (&repair));

  /* Older than the history: full rewrite. */
  for (int i = 0; i < XS_DAMAGE_HISTORY; i++)
    xs_damage_tracker_add_frame (&t, &b);
  xs_damage_tracker_get_repair (&t, 0, &repair);
  assert (repair.full);
}

int
main (void)
{
  test_region_basics ();
  test_region_collapse ();
  test_tracker ();
  printf ("xs-damage: all tests passed\n");
  return 0;
}
