/*
 * xs-input: injects remote input into GNOME through the RemoteDesktop
 * session's EIS (libei) connection.
 *
 * Absolute pointer positions are in the virtual monitor's stream pixels and
 * go to the EIS device whose region carries the stream's mapping id.
 */
#pragma once

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../protocol/xs-protocol.h"

typedef struct _XsInput XsInput;

/* Reports where the virtual monitor sits in the compositor's logical
 * coordinate space, once the EIS device for it shows up. */
typedef void (*XsInputRegionFunc) (double x, double y, double width, double height,
                                   void *user_data);

/* `rd_session_path` must be started; `stream_path` is the screen cast
 * stream input should map to. Hooks into the default GMainContext. */
XsInput *xs_input_new (GDBusConnection    *bus,
                       const char         *rd_session_path,
                       const char         *stream_path,
                       XsInputRegionFunc   region_func,
                       void               *user_data,
                       GError            **error);

void xs_input_free (XsInput *input);

void xs_input_handle (XsInput            *input,
                      const XsInputEvent *event);
