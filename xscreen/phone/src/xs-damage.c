#include "xs-damage.h"

#include <string.h>

void
xs_region_clear (XsRegion *region)
{
  region->n_rects = 0;
  region->full = false;
}

void
xs_region_set_full (XsRegion *region)
{
  region->n_rects = 0;
  region->full = true;
}

bool
xs_region_is_empty (const XsRegion *region)
{
  return !region->full && region->n_rects == 0;
}

static void
collapse_to_bounds (XsRegion *region)
{
  int x1 = region->rects[0].x;
  int y1 = region->rects[0].y;
  int x2 = x1 + region->rects[0].width;
  int y2 = y1 + region->rects[0].height;

  for (int i = 1; i < region->n_rects; i++)
    {
      const XsRect *r = &region->rects[i];

      if (r->x < x1) x1 = r->x;
      if (r->y < y1) y1 = r->y;
      if (r->x + r->width > x2) x2 = r->x + r->width;
      if (r->y + r->height > y2) y2 = r->y + r->height;
    }
  region->rects[0] = (XsRect) { x1, y1, x2 - x1, y2 - y1 };
  region->n_rects = 1;
}

static bool
contains (const XsRect *outer, const XsRect *inner)
{
  return inner->x >= outer->x && inner->y >= outer->y &&
         inner->x + inner->width <= outer->x + outer->width &&
         inner->y + inner->height <= outer->y + outer->height;
}

void
xs_region_add (XsRegion     *region,
               const XsRect *rect,
               int           width,
               int           height)
{
  int x1 = rect->x < 0 ? 0 : rect->x;
  int y1 = rect->y < 0 ? 0 : rect->y;
  int x2 = rect->x + rect->width > width ? width : rect->x + rect->width;
  int y2 = rect->y + rect->height > height ? height : rect->y + rect->height;
  XsRect clipped;

  if (region->full || x2 <= x1 || y2 <= y1)
    return;

  clipped = (XsRect) { x1, y1, x2 - x1, y2 - y1 };
  if (clipped.width == width && clipped.height == height)
    {
      xs_region_set_full (region);
      return;
    }

  for (int i = 0; i < region->n_rects; i++)
    if (contains (&region->rects[i], &clipped))
      return;

  if (region->n_rects == XS_REGION_MAX_RECTS)
    collapse_to_bounds (region);
  region->rects[region->n_rects++] = clipped;
}

void
xs_region_union (XsRegion       *dest,
                 const XsRegion *src,
                 int             width,
                 int             height)
{
  if (src->full)
    {
      xs_region_set_full (dest);
      return;
    }
  for (int i = 0; i < src->n_rects && !dest->full; i++)
    xs_region_add (dest, &src->rects[i], width, height);
}

int64_t
xs_region_area (const XsRegion *region, int width, int height)
{
  int64_t area = 0;

  if (region->full)
    return (int64_t) width * height;
  for (int i = 0; i < region->n_rects; i++)
    area += (int64_t) region->rects[i].width * region->rects[i].height;
  return area;
}

void
xs_damage_tracker_reset (XsDamageTracker *tracker, int width, int height)
{
  memset (tracker, 0, sizeof *tracker);
  tracker->width = width;
  tracker->height = height;
}

void
xs_damage_tracker_add_frame (XsDamageTracker *tracker, const XsRegion *damage)
{
  tracker->sequence++;
  tracker->history[(tracker->sequence - 1) % XS_DAMAGE_HISTORY] = *damage;
}

void
xs_damage_tracker_get_repair (const XsDamageTracker *tracker,
                              int                    slot,
                              XsRegion              *repair)
{
  uint64_t held = tracker->slot_sequence[slot];

  xs_region_clear (repair);

  /* Never written, or too old for the history: rewrite all of it. */
  if (held == 0 || tracker->sequence - held >= XS_DAMAGE_HISTORY)
    {
      xs_region_set_full (repair);
      return;
    }

  for (uint64_t frame = held + 1; frame <= tracker->sequence && !repair->full; frame++)
    xs_region_union (repair, &tracker->history[(frame - 1) % XS_DAMAGE_HISTORY],
                     tracker->width, tracker->height);
}

void
xs_damage_tracker_mark_slot (XsDamageTracker *tracker, int slot)
{
  tracker->slot_sequence[slot] = tracker->sequence;
}
