#include "xs-layout.h"

#include <math.h>
#include <stdbool.h>

#define DC "org.gnome.Mutter.DisplayConfig"

/* Logical size of the monitor `connector` in the current mode at `scale`. */
static bool
monitor_logical_size (GVariant   *monitors,
                      const char *connector,
                      double      scale,
                      uint32_t    transform,
                      double     *width,
                      double     *height)
{
  GVariantIter iter;
  GVariant *monitor;

  g_variant_iter_init (&iter, monitors);
  while ((monitor = g_variant_iter_next_value (&iter)))
    {
      g_autoptr (GVariant) spec = g_variant_get_child_value (monitor, 0);
      g_autoptr (GVariant) modes = g_variant_get_child_value (monitor, 1);
      const char *name = NULL;
      GVariantIter mode_iter;
      GVariant *mode;

      g_variant_get_child (spec, 0, "&s", &name);
      if (g_strcmp0 (name, connector) != 0)
        {
          g_variant_unref (monitor);
          continue;
        }

      g_variant_iter_init (&mode_iter, modes);
      while ((mode = g_variant_iter_next_value (&mode_iter)))
        {
          g_autoptr (GVariant) props = g_variant_get_child_value (mode, 6);
          gboolean current = FALSE;
          int32_t w, h;

          g_variant_lookup (props, "is-current", "b", &current);
          g_variant_get_child (mode, 1, "i", &w);
          g_variant_get_child (mode, 2, "i", &h);
          g_variant_unref (mode);
          if (!current)
            continue;
          /* Transforms 1 and 3 (and their flipped variants) rotate by 90. */
          if (transform % 2)
            {
              int32_t t = w;

              w = h;
              h = t;
            }
          *width = w / scale;
          *height = h / scale;
          g_variant_unref (monitor);
          return true;
        }
      g_variant_unref (monitor);
    }
  return false;
}

static bool
overlaps (double a1, double a2, double b1, double b2)
{
  return a1 < b2 && b1 < a2;
}

static bool
near (double a, double b)
{
  return fabs (a - b) < 0.5;
}

uint32_t
xs_layout_get_edges (GDBusConnection *bus, const XsLogicalRect *ours)
{
  g_autoptr (GVariant) state = NULL;
  g_autoptr (GVariant) monitors = NULL;
  g_autoptr (GVariant) logical_monitors = NULL;
  GVariantIter iter;
  GVariant *lm;
  uint32_t edges = 0;

  state = g_dbus_connection_call_sync (bus, DC, "/org/gnome/Mutter/DisplayConfig", DC,
                                       "GetCurrentState", NULL, NULL, 0, -1, NULL, NULL);
  if (!state)
    return 0;
  monitors = g_variant_get_child_value (state, 1);
  logical_monitors = g_variant_get_child_value (state, 2);

  g_variant_iter_init (&iter, logical_monitors);
  while ((lm = g_variant_iter_next_value (&iter)))
    {
      g_autoptr (GVariant) specs = g_variant_get_child_value (lm, 5);
      int32_t x, y;
      double scale, width = 0, height = 0;
      uint32_t transform;
      const char *connector = NULL;

      g_variant_get_child (lm, 0, "i", &x);
      g_variant_get_child (lm, 1, "i", &y);
      g_variant_get_child (lm, 2, "d", &scale);
      g_variant_get_child (lm, 3, "u", &transform);
      if (g_variant_n_children (specs) > 0)
        {
          g_autoptr (GVariant) spec = g_variant_get_child_value (specs, 0);

          g_variant_get_child (spec, 0, "&s", &connector);
          monitor_logical_size (monitors, connector, scale, transform, &width, &height);
        }
      g_variant_unref (lm);

      if (width <= 0 || (near (x, ours->x) && near (y, ours->y)))
        continue;   /* unknown, or our own monitor */

      if (near (x + width, ours->x) && overlaps (y, y + height, ours->y, ours->y + ours->height))
        edges |= XS_EDGE_LEFT;
      if (near (x, ours->x + ours->width) && overlaps (y, y + height, ours->y, ours->y + ours->height))
        edges |= XS_EDGE_RIGHT;
      if (near (y + height, ours->y) && overlaps (x, x + width, ours->x, ours->x + ours->width))
        edges |= XS_EDGE_TOP;
      if (near (y, ours->y + ours->height) && overlaps (x, x + width, ours->x, ours->x + ours->width))
        edges |= XS_EDGE_BOTTOM;
    }
  return edges;
}
