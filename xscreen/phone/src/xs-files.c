#include "xs-files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- sending ------------------------------------------------------------ */

typedef struct
{
  char **paths;
  uint32_t purpose;
} Job;

struct _XsFileSender
{
  XsFilesSendFunc send;
  void *user_data;
  GAsyncQueue *jobs;
  GThread *thread;
  atomic_bool stopping;
  uint32_t next_transfer;
  uint8_t *chunk;
};

static Job stop_job;   /* tells the thread to exit */

static void
job_free (Job *job)
{
  if (job == &stop_job)
    return;
  g_strfreev (job->paths);
  g_free (job);
}

/* Sizes up `path`: false if it holds nothing that can be sent. */
static bool
measure (const char *path, uint64_t *total)
{
  struct stat st;

  if (stat (path, &st) < 0)
    return false;
  if (S_ISREG (st.st_mode))
    {
      *total += st.st_size;
      return true;
    }
  if (S_ISDIR (st.st_mode))
    {
      g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          struct stat lst;

          /* Links to folders could loop; links to files are followed. */
          if (lstat (child, &lst) == 0 && S_ISLNK (lst.st_mode) &&
              stat (child, &lst) == 0 && S_ISDIR (lst.st_mode))
            continue;
          measure (child, total);
        }
      return true;
    }
  return false;
}

static bool
send_entry (XsFileSender *sender, uint32_t transfer, uint32_t purpose,
            const char *path, const char *relative)
{
  XsFileBegin begin = { .transfer = transfer, .purpose = purpose };
  XsFileData data = { .transfer = transfer };
  struct stat st;
  int fd;

  if (atomic_load (&sender->stopping) || stat (path, &st) < 0)
    return false;

  if (S_ISDIR (st.st_mode))
    {
      g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      const char *name;

      begin.flags = XS_FILE_DIRECTORY;
      sender->send (XS_MSG_FILE_BEGIN, &begin, sizeof begin, relative, strlen (relative),
                    sender->user_data);
      while (dir && (name = g_dir_read_name (dir)))
        g_ptr_array_add (names, g_strdup (name));
      g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
      for (guint i = 0; i < names->len; i++)
        {
          g_autofree char *child = g_build_filename (path, names->pdata[i], NULL);
          g_autofree char *child_relative = g_strconcat (relative, "/", names->pdata[i], NULL);
          struct stat lst;

          if (lstat (child, &lst) == 0 && S_ISLNK (lst.st_mode) &&
              stat (child, &lst) == 0 && S_ISDIR (lst.st_mode))
            continue;
          if (!send_entry (sender, transfer, purpose, child, child_relative))
            return false;
        }
      return true;
    }
  if (!S_ISREG (st.st_mode))
    return true;   /* devices, sockets: skipped */

  fd = open (path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  begin.size = st.st_size;
  sender->send (XS_MSG_FILE_BEGIN, &begin, sizeof begin, relative, strlen (relative),
                sender->user_data);
  for (uint64_t left = st.st_size; left > 0;)
    {
      ssize_t n = read (fd, sender->chunk, MIN (left, XS_FILE_CHUNK));

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0 || atomic_load (&sender->stopping))
        {
          close (fd);
          return false;
        }
      sender->send (XS_MSG_FILE_DATA, &data, sizeof data, sender->chunk, n, sender->user_data);
      left -= n;
    }
  close (fd);
  return true;
}

static gpointer
sender_thread (gpointer user_data)
{
  XsFileSender *sender = user_data;
  Job *job;

  while ((job = g_async_queue_pop (sender->jobs)) != &stop_job)
    {
      XsFileEnd end = { .transfer = ++sender->next_transfer };
      bool ok = true;

      for (int i = 0; ok && job->paths[i]; i++)
        {
          g_autofree char *name = g_path_get_basename (job->paths[i]);
          ok = send_entry (sender, end.transfer, job->purpose, job->paths[i], name);
        }
      end.status = ok ? 0 : 1;
      sender->send (XS_MSG_FILE_END, &end, sizeof end, NULL, 0, sender->user_data);
      job_free (job);
    }
  return NULL;
}

XsFileSender *
xs_file_sender_new (XsFilesSendFunc send, void *user_data)
{
  XsFileSender *sender = g_new0 (XsFileSender, 1);

  sender->send = send;
  sender->user_data = user_data;
  sender->chunk = g_malloc (XS_FILE_CHUNK);
  sender->jobs = g_async_queue_new_full ((GDestroyNotify) job_free);
  sender->thread = g_thread_new ("xs-files", sender_thread, sender);
  return sender;
}

void
xs_file_sender_free (XsFileSender *sender)
{
  if (!sender)
    return;
  atomic_store (&sender->stopping, true);
  g_async_queue_push_front (sender->jobs, &stop_job);
  g_thread_join (sender->thread);
  g_async_queue_unref (sender->jobs);
  g_free (sender->chunk);
  g_free (sender);
}

bool
xs_file_sender_send (XsFileSender *sender, const char *const *paths, uint32_t purpose,
                     uint64_t max_bytes, GError **error)
{
  uint64_t total = 0;
  Job *job;

  for (int i = 0; paths[i]; i++)
    if (!measure (paths[i], &total))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "cannot read %s", paths[i]);
        return false;
      }
  if (max_bytes && total > max_bytes)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                   "%" G_GUINT64_FORMAT " MiB is more than the %" G_GUINT64_FORMAT " MiB limit",
                   total >> 20, max_bytes >> 20);
      return false;
    }
  job = g_new0 (Job, 1);
  job->paths = g_strdupv ((char **) paths);
  job->purpose = purpose;
  g_async_queue_push (sender->jobs, job);
  return true;
}

/* --- receiving ---------------------------------------------------------- */

struct _XsFileReceiver
{
  XsFilesReceivedFunc received;
  void *user_data;
  bool active;
  bool failed;
  uint32_t transfer;
  uint32_t purpose;
  char *base;               /* destination folder */
  GHashTable *tops;         /* sent top-level name -> local absolute path */
  GPtrArray *top_order;     /* local absolute paths, in arrival order */
  int fd;
  uint64_t left;            /* bytes still due for the open file */
};

static void
remove_tree (const char *path)
{
  struct stat st;

  if (lstat (path, &st) < 0)
    return;
  if (S_ISDIR (st.st_mode))
    {
      g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          remove_tree (child);
        }
      rmdir (path);
    }
  else
    unlink (path);
}

static char *
clipboard_root (void)
{
  return g_build_filename (g_get_user_cache_dir (), "xscreen", "clipboard", NULL);
}

static void
reset (XsFileReceiver *receiver, bool keep)
{
  if (receiver->fd >= 0)
    close (receiver->fd);
  receiver->fd = -1;
  receiver->left = 0;
  if (!keep)
    for (guint i = 0; i < receiver->top_order->len; i++)
      remove_tree (receiver->top_order->pdata[i]);
  g_hash_table_remove_all (receiver->tops);
  g_ptr_array_set_size (receiver->top_order, 0);
  g_clear_pointer (&receiver->base, g_free);
  receiver->active = false;
  receiver->failed = false;
}

static bool
start (XsFileReceiver *receiver, uint32_t transfer, uint32_t purpose)
{
  if (receiver->active)
    reset (receiver, false);
  receiver->active = true;
  receiver->transfer = transfer;
  receiver->purpose = purpose;
  if (purpose == XS_FILE_FOR_CLIPBOARD)
    {
      g_autofree char *root = clipboard_root ();

      /* Only the latest copy is kept. */
      remove_tree (root);
      receiver->base = g_strdup_printf ("%s/%u", root, transfer);
    }
  else
    {
      const char *downloads = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);

      receiver->base = downloads ? g_strdup (downloads) :
                       g_build_filename (g_get_home_dir (), "Downloads", NULL);
    }
  return g_mkdir_with_parents (receiver->base, 0700) == 0;
}

/* A free name for a new top-level entry: "name", "name (2)", ... */
static char *
unused_path (const char *base, const char *name)
{
  const char *dot = strrchr (name, '.');
  g_autofree char *stem = g_strndup (name, dot && dot != name ? (gsize) (dot - name) : strlen (name));
  const char *ext = dot && dot != name ? dot : "";
  char *path = g_build_filename (base, name, NULL);

  for (int n = 2; g_file_test (path, G_FILE_TEST_EXISTS); n++)
    {
      g_autofree char *candidate = g_strdup_printf ("%s (%d)%s", stem, n, ext);
      g_free (path);
      path = g_build_filename (base, candidate, NULL);
    }
  return path;
}

/* The local path for a sent relative path, or NULL if it is unsafe. */
static char *
local_path (XsFileReceiver *receiver, const char *relative)
{
  g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
  const char *top;

  if (!parts[0] || relative[0] == '/')
    return NULL;
  for (int i = 0; parts[i]; i++)
    if (!parts[i][0] || !strcmp (parts[i], ".") || !strcmp (parts[i], ".."))
      return NULL;

  top = g_hash_table_lookup (receiver->tops, parts[0]);
  if (!top)
    {
      char *path = receiver->purpose == XS_FILE_FOR_CLIPBOARD ?
                   g_build_filename (receiver->base, parts[0], NULL) :
                   unused_path (receiver->base, parts[0]);

      g_hash_table_insert (receiver->tops, g_strdup (parts[0]), path);
      g_ptr_array_add (receiver->top_order, g_strdup (path));
      top = path;
    }
  if (!parts[1])
    return g_strdup (top);
  {
    g_autofree char *rest = g_strjoinv ("/", parts + 1);
    return g_build_filename (top, rest, NULL);
  }
}

static bool
handle_begin (XsFileReceiver *receiver, const XsFileBegin *begin, const char *relative)
{
  g_autofree char *path = NULL;
  g_autofree char *parent = NULL;

  if (!receiver->active || begin->transfer != receiver->transfer)
    {
      if (!start (receiver, begin->transfer, begin->purpose))
        receiver->failed = true;
    }
  if (receiver->left)
    receiver->failed = true;    /* previous file cut short */
  if (receiver->fd >= 0)
    close (receiver->fd);
  receiver->fd = -1;
  receiver->left = 0;
  if (receiver->failed)
    return true;

  path = local_path (receiver, relative);
  if (!path)
    return false;
  if (begin->flags & XS_FILE_DIRECTORY)
    {
      if (g_mkdir_with_parents (path, 0755) < 0)
        receiver->failed = true;
      return true;
    }
  parent = g_path_get_dirname (path);
  g_mkdir_with_parents (parent, 0755);
  receiver->fd = open (path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (receiver->fd < 0)
    {
      fprintf (stderr, "xs-files: %s: %s\n", path, g_strerror (errno));
      receiver->failed = true;
      return true;
    }
  receiver->left = begin->size;
  if (!receiver->left)
    {
      close (receiver->fd);
      receiver->fd = -1;
    }
  return true;
}

static bool
handle_data (XsFileReceiver *receiver, const uint8_t *data, size_t size)
{
  if (receiver->failed)
    return true;
  if (receiver->fd < 0 || size > receiver->left)
    return false;
  while (size)
    {
      ssize_t n = write (receiver->fd, data, size);

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        {
          fprintf (stderr, "xs-files: write: %s\n", g_strerror (errno));
          receiver->failed = true;
          return true;
        }
      data += n;
      size -= n;
      receiver->left -= n;
    }
  if (!receiver->left)
    {
      close (receiver->fd);
      receiver->fd = -1;
    }
  return true;
}

static void
handle_end (XsFileReceiver *receiver, const XsFileEnd *end)
{
  bool ok;

  if (!receiver->active || end->transfer != receiver->transfer)
    return;
  ok = end->status == 0 && !receiver->failed && !receiver->left;
  if (ok && receiver->top_order->len)
    {
      g_ptr_array_add (receiver->top_order, NULL);
      receiver->received (receiver->purpose, (const char *const *) receiver->top_order->pdata,
                          receiver->user_data);
      g_ptr_array_remove_index (receiver->top_order, receiver->top_order->len - 1);
    }
  reset (receiver, ok);
}

XsFileReceiver *
xs_file_receiver_new (XsFilesReceivedFunc received, void *user_data)
{
  XsFileReceiver *receiver = g_new0 (XsFileReceiver, 1);

  receiver->received = received;
  receiver->user_data = user_data;
  receiver->fd = -1;
  receiver->tops = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  receiver->top_order = g_ptr_array_new_with_free_func (g_free);
  return receiver;
}

void
xs_file_receiver_free (XsFileReceiver *receiver)
{
  if (!receiver)
    return;
  reset (receiver, false);
  g_hash_table_unref (receiver->tops);
  g_ptr_array_unref (receiver->top_order);
  g_free (receiver);
}

bool
xs_file_receiver_handle (XsFileReceiver *receiver, uint16_t type,
                         const uint8_t *payload, size_t size)
{
  switch (type)
    {
    case XS_MSG_CLIENT_FILE_BEGIN:
      {
        XsFileBegin begin;
        g_autofree char *relative = NULL;

        if (size < sizeof begin)
          return false;
        memcpy (&begin, payload, sizeof begin);
        relative = g_strndup ((const char *) payload + sizeof begin, size - sizeof begin);
        if (!g_utf8_validate (relative, -1, NULL))
          return false;
        return handle_begin (receiver, &begin, relative);
      }
    case XS_MSG_CLIENT_FILE_DATA:
      {
        XsFileData data;

        if (size < sizeof data)
          return false;
        memcpy (&data, payload, sizeof data);
        if (!receiver->active || data.transfer != receiver->transfer)
          return true;
        return handle_data (receiver, payload + sizeof data, size - sizeof data);
      }
    case XS_MSG_CLIENT_FILE_END:
      {
        XsFileEnd end;

        if (size < sizeof end)
          return false;
        memcpy (&end, payload, sizeof end);
        handle_end (receiver, &end);
        return true;
      }
    default:
      return false;
    }
}
