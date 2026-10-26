/*
 * xs-clipboard: clipboard sharing through the RemoteDesktop session
 * (EnableClipboard, SelectionRead/Write), so a remote client's clipboard
 * and GNOME's stay in step. Carries text, PNG images and lists of files.
 */
#pragma once

#include <gio/gio.h>

typedef struct _XsClipboard XsClipboard;

typedef enum
{
  XS_CLIP_TEXT,     /* UTF-8 */
  XS_CLIP_IMAGE,    /* PNG */
  XS_CLIP_FILES,    /* text/uri-list of local files */
} XsClipKind;

/* Called on the main context when another application copies something. */
typedef void (*XsClipboardChangedFunc) (XsClipKind kind, GBytes *content, void *user_data);

/* `rd_session_path` must be a started RemoteDesktop session. */
XsClipboard *xs_clipboard_new (GDBusConnection        *bus,
                               const char             *rd_session_path,
                               XsClipboardChangedFunc  changed,
                               void                   *user_data,
                               GError                **error);

void xs_clipboard_free (XsClipboard *clipboard);

/* Makes `content` GNOME's clipboard content. */
void xs_clipboard_set (XsClipboard *clipboard, XsClipKind kind, GBytes *content);
void xs_clipboard_set_text (XsClipboard *clipboard, const char *text);

/* The current clipboard text, or NULL. */
char *xs_clipboard_get_text (XsClipboard *clipboard);
