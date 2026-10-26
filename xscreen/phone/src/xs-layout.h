/*
 * xs-layout: which edges of the virtual monitor touch other monitors in
 * the GNOME layout (org.gnome.Mutter.DisplayConfig), so the client knows
 * where the pointer may cross over.
 */
#pragma once

#include <gio/gio.h>
#include <stdint.h>

#include "../../protocol/xs-protocol.h"

typedef struct
{
  double x;
  double y;
  double width;
  double height;
} XsLogicalRect;

/* Returns a mask of XS_EDGE_* for `ours` (logical coordinates). */
uint32_t xs_layout_get_edges (GDBusConnection     *bus,
                              const XsLogicalRect *ours);
