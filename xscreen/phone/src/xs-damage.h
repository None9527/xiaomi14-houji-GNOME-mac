/*
 * xs-damage: damage regions and per-buffer repair tracking.
 *
 * The encoder cycles through a handful of NV12 input buffers, so a buffer
 * picked for the next frame holds an older frame. Like EGL buffer age, the
 * tracker remembers each frame's damage and tells which area a given buffer
 * has to be brought up to date with.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define XS_REGION_MAX_RECTS 16
#define XS_DAMAGE_HISTORY 16
#define XS_DAMAGE_MAX_SLOTS 16

typedef struct
{
  int x;
  int y;
  int width;
  int height;
} XsRect;

/* A small set of rectangles; past XS_REGION_MAX_RECTS it collapses to the
 * bounding box, which over-covers but never under-covers. */
typedef struct
{
  XsRect rects[XS_REGION_MAX_RECTS];
  int n_rects;
  bool full;
} XsRegion;

void xs_region_clear (XsRegion *region);

void xs_region_set_full (XsRegion *region);

/* Adds `rect` clipped to a `width` x `height` frame. */
void xs_region_add (XsRegion     *region,
                    const XsRect *rect,
                    int           width,
                    int           height);

void xs_region_union (XsRegion       *dest,
                      const XsRegion *src,
                      int             width,
                      int             height);

bool xs_region_is_empty (const XsRegion *region);

/* Total covered area in pixels (overlaps counted twice). */
int64_t xs_region_area (const XsRegion *region,
                        int             width,
                        int             height);

typedef struct
{
  int width;
  int height;
  uint64_t sequence;                          /* frames recorded so far */
  XsRegion history[XS_DAMAGE_HISTORY];        /* damage of recent frames */
  uint64_t slot_sequence[XS_DAMAGE_MAX_SLOTS]; /* frame each slot holds, 0: none */
} XsDamageTracker;

/* Forgets all history; every slot needs a full conversion afterwards. */
void xs_damage_tracker_reset (XsDamageTracker *tracker,
                              int              width,
                              int              height);

/* Records the damage of a new frame. */
void xs_damage_tracker_add_frame (XsDamageTracker *tracker,
                                  const XsRegion  *damage);

/* Area `slot` must be rewritten so it matches the newest frame. */
void xs_damage_tracker_get_repair (const XsDamageTracker *tracker,
                                   int                    slot,
                                   XsRegion              *repair);

/* Marks `slot` as now holding the newest frame. */
void xs_damage_tracker_mark_slot (XsDamageTracker *tracker,
                                  int              slot);
