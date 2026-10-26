#include "xs-clipboard.h"

#include <errno.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RD_SESSION "org.gnome.Mutter.RemoteDesktop.Session"
#define RD_NAME "org.gnome.Mutter.RemoteDesktop"
#define MAX_CONTENT (32 * 1024 * 1024)
#define READ_TIMEOUT_MS 1000

static const char *const text_mime_types[] = {
  "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT", NULL,
};
static const char *const image_mime_types[] = { "image/png", NULL };
/* File managers paste the GNOME list; other applications take a URI list
 * or, as text, the paths. */
static const char *const files_mime_types[] = {
  "x-special/gnome-copied-files", "text/uri-list", "text/plain;charset=utf-8", NULL,
};

struct _XsClipboard
{
  GDBusConnection *bus;
  char *session_path;
  XsClipboardChangedFunc changed;
  void *user_data;
  guint owner_changed_id;
  guint transfer_id;
  /* What we own, served on SelectionTransfer. */
  XsClipKind offered_kind;
  GBytes *offered;
  char *text_mime;          /* best text type the current owner offers */
};

static GVariant *
call_with_fd (XsClipboard *clipboard, const char *method, GVariant *params, int *fd)
{
  g_autoptr (GUnixFDList) fds = NULL;
  g_autoptr (GError) error = NULL;
  GVariant *reply;
  gint32 index;

  reply = g_dbus_connection_call_with_unix_fd_list_sync (clipboard->bus, RD_NAME,
                                                         clipboard->session_path, RD_SESSION,
                                                         method, params, G_VARIANT_TYPE ("(h)"),
                                                         0, -1, NULL, &fds, NULL, &error);
  if (!reply)
    {
      fprintf (stderr, "xs-clipboard: %s: %s\n", method, error->message);
      return NULL;
    }
  g_variant_get (reply, "(h)", &index);
  *fd = g_unix_fd_list_get (fds, index, &error);
  if (*fd < 0)
    {
      g_variant_unref (reply);
      return NULL;
    }
  return reply;
}

static GBytes *
read_selection (XsClipboard *clipboard, const char *mime)
{
  g_autoptr (GVariant) reply = NULL;
  GByteArray *data = g_byte_array_new ();
  int fd = -1;

  reply = call_with_fd (clipboard, "SelectionRead", g_variant_new ("(s)", mime), &fd);
  if (!reply)
    return g_byte_array_free_to_bytes (data);
  for (;;)
    {
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      uint8_t chunk[65536];
      ssize_t n;

      if (poll (&pfd, 1, READ_TIMEOUT_MS) <= 0)
        break;
      n = read (fd, chunk, sizeof chunk);
      if (n <= 0)
        break;
      if (data->len + n > MAX_CONTENT)
        {
          g_byte_array_set_size (data, 0);
          break;
        }
      g_byte_array_append (data, chunk, n);
    }
  close (fd);
  return g_byte_array_free_to_bytes (data);
}

static const char *
first_offered (const char *const *offered, const char *const *wanted)
{
  for (int i = 0; wanted[i]; i++)
    if (g_strv_contains (offered, wanted[i]))
      return wanted[i];
  return NULL;
}

/* The local files in a URI list (or GNOME's "copy\nuri..." list), as a
 * URI list; NULL if it names none. */
static GBytes *
local_uri_list (GBytes *content)
{
  g_autofree char *text = g_strndup (g_bytes_get_data (content, NULL), g_bytes_get_size (content));
  g_auto (GStrv) lines = g_strsplit_set (text, "\r\n", -1);
  GString *list = g_string_new (NULL);

  for (int i = 0; lines[i]; i++)
    if (g_str_has_prefix (lines[i], "file://"))
      g_string_append_printf (list, "%s\r\n", lines[i]);
  if (!list->len)
    {
      g_string_free (list, TRUE);
      return NULL;
    }
  return g_string_free_to_bytes (list);
}

static void
on_owner_changed (GDBusConnection *connection, const char *sender, const char *path,
                  const char *iface, const char *signal, GVariant *params, gpointer user_data)
{
  XsClipboard *clipboard = user_data;
  g_autoptr (GVariant) options = NULL;
  g_autofree const char **mimes = NULL;
  g_autoptr (GBytes) content = NULL;
  gboolean ours = FALSE;
  const char *mime;

  g_clear_pointer (&clipboard->text_mime, g_free);
  g_variant_get (params, "(@a{sv})", &options);
  g_variant_lookup (options, "session-is-owner", "b", &ours);
  if (ours || !g_variant_lookup (options, "mime-types", "^a&s", &mimes))
    return;
  /* Another application owns the clipboard now. */
  g_clear_pointer (&clipboard->offered, g_bytes_unref);
  mime = first_offered (mimes, text_mime_types);
  clipboard->text_mime = g_strdup (mime);

  if ((mime = first_offered (mimes, (const char *[]) { "x-special/gnome-copied-files",
                                                       "text/uri-list", NULL })))
    {
      g_autoptr (GBytes) list = read_selection (clipboard, mime);
      if ((content = local_uri_list (list)))
        {
          clipboard->changed (XS_CLIP_FILES, content, clipboard->user_data);
          return;
        }
    }
  if ((mime = first_offered (mimes, image_mime_types)))
    {
      content = read_selection (clipboard, mime);
      if (g_bytes_get_size (content))
        {
          clipboard->changed (XS_CLIP_IMAGE, content, clipboard->user_data);
          return;
        }
      g_clear_pointer (&content, g_bytes_unref);
    }
  if (clipboard->text_mime)
    {
      content = read_selection (clipboard, clipboard->text_mime);
      if (g_bytes_get_size (content) &&
          g_utf8_validate (g_bytes_get_data (content, NULL), g_bytes_get_size (content), NULL))
        clipboard->changed (XS_CLIP_TEXT, content, clipboard->user_data);
    }
}

/* The offered content in the form `mime` asks for. */
static GBytes *
offered_as (XsClipboard *clipboard, const char *mime)
{
  g_autofree char *list = NULL;
  g_auto (GStrv) uris = NULL;
  GString *out;

  if (clipboard->offered_kind != XS_CLIP_FILES || !strcmp (mime, "text/uri-list"))
    return g_bytes_ref (clipboard->offered);

  list = g_strndup (g_bytes_get_data (clipboard->offered, NULL),
                    g_bytes_get_size (clipboard->offered));
  uris = g_uri_list_extract_uris (list);
  out = g_string_new (!strcmp (mime, "x-special/gnome-copied-files") ? "copy" : NULL);
  for (int i = 0; uris[i]; i++)
    {
      g_autofree char *path = NULL;

      if (out->len)
        g_string_append_c (out, '\n');
      if (!strcmp (mime, "x-special/gnome-copied-files") ||
          !(path = g_filename_from_uri (uris[i], NULL, NULL)))
        g_string_append (out, uris[i]);
      else
        g_string_append (out, path);
    }
  return g_string_free_to_bytes (out);
}

static void
on_transfer (GDBusConnection *connection, const char *sender, const char *path,
             const char *iface, const char *signal, GVariant *params, gpointer user_data)
{
  XsClipboard *clipboard = user_data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GBytes) content = NULL;
  const char *mime;
  guint32 serial;
  gboolean ok = FALSE;
  int fd = -1;

  g_variant_get (params, "(&su)", &mime, &serial);
  if (clipboard->offered)
    reply = call_with_fd (clipboard, "SelectionWrite", g_variant_new ("(u)", serial), &fd);
  if (reply)
    {
      size_t len, done = 0;
      const uint8_t *data;

      content = offered_as (clipboard, mime);
      data = g_bytes_get_data (content, &len);
      while (done < len)
        {
          ssize_t n = write (fd, data + done, len - done);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            break;
          done += n;
        }
      ok = done == len;
      close (fd);
    }
  g_dbus_connection_call (clipboard->bus, RD_NAME, clipboard->session_path, RD_SESSION,
                          "SelectionWriteDone", g_variant_new ("(ub)", serial, ok),
                          NULL, 0, -1, NULL, NULL, NULL);
}

XsClipboard *
xs_clipboard_new (GDBusConnection *bus, const char *rd_session_path,
                  XsClipboardChangedFunc changed, void *user_data, GError **error)
{
  XsClipboard *clipboard = g_new0 (XsClipboard, 1);
  g_autoptr (GVariant) reply = NULL;
  GVariantBuilder options;

  clipboard->bus = g_object_ref (bus);
  clipboard->session_path = g_strdup (rd_session_path);
  clipboard->changed = changed;
  clipboard->user_data = user_data;

  clipboard->owner_changed_id =
    g_dbus_connection_signal_subscribe (bus, NULL, RD_SESSION, "SelectionOwnerChanged",
                                        rd_session_path, NULL, 0, on_owner_changed,
                                        clipboard, NULL);
  clipboard->transfer_id =
    g_dbus_connection_signal_subscribe (bus, NULL, RD_SESSION, "SelectionTransfer",
                                        rd_session_path, NULL, 0, on_transfer, clipboard, NULL);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  reply = g_dbus_connection_call_sync (bus, RD_NAME, rd_session_path, RD_SESSION,
                                       "EnableClipboard", g_variant_new ("(a{sv})", &options),
                                       NULL, 0, -1, NULL, error);
  if (!reply)
    {
      xs_clipboard_free (clipboard);
      return NULL;
    }
  return clipboard;
}

void
xs_clipboard_free (XsClipboard *clipboard)
{
  if (!clipboard)
    return;
  g_dbus_connection_signal_unsubscribe (clipboard->bus, clipboard->owner_changed_id);
  g_dbus_connection_signal_unsubscribe (clipboard->bus, clipboard->transfer_id);
  g_object_unref (clipboard->bus);
  g_free (clipboard->session_path);
  g_clear_pointer (&clipboard->offered, g_bytes_unref);
  g_free (clipboard->text_mime);
  g_free (clipboard);
}

void
xs_clipboard_set (XsClipboard *clipboard, XsClipKind kind, GBytes *content)
{
  const char *const *mimes = kind == XS_CLIP_FILES ? files_mime_types :
                             kind == XS_CLIP_IMAGE ? image_mime_types : text_mime_types;
  GVariantBuilder options;

  g_clear_pointer (&clipboard->offered, g_bytes_unref);
  clipboard->offered = g_bytes_ref (content);
  clipboard->offered_kind = kind;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "mime-types", g_variant_new_strv (mimes, -1));
  /* Synchronous, so a paste injected right after sees the new content. */
  g_autoptr (GVariant) reply =
    g_dbus_connection_call_sync (clipboard->bus, RD_NAME, clipboard->session_path, RD_SESSION,
                                 "SetSelection", g_variant_new ("(a{sv})", &options),
                                 NULL, 0, -1, NULL, NULL);
}

void
xs_clipboard_set_text (XsClipboard *clipboard, const char *text)
{
  g_autoptr (GBytes) content = g_bytes_new (text, strlen (text));

  xs_clipboard_set (clipboard, XS_CLIP_TEXT, content);
}

char *
xs_clipboard_get_text (XsClipboard *clipboard)
{
  g_autoptr (GBytes) content = NULL;

  if (clipboard->offered)
    return clipboard->offered_kind == XS_CLIP_TEXT ?
      g_strndup (g_bytes_get_data (clipboard->offered, NULL),
                 g_bytes_get_size (clipboard->offered)) : NULL;
  if (!clipboard->text_mime)
    return NULL;
  content = read_selection (clipboard, clipboard->text_mime);
  if (!g_utf8_validate (g_bytes_get_data (content, NULL), g_bytes_get_size (content), NULL))
    return NULL;
  return g_strndup (g_bytes_get_data (content, NULL), g_bytes_get_size (content));
}
