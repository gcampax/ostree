/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include <gio/gunixoutputstream.h>

#define DEFAULT_PACK_SIZE_BYTES (50*1024*1024)

static char* opt_pack_size;
static char* opt_compression;

static GOptionEntry options[] = {
  { "pack-size", 0, 0, G_OPTION_ARG_STRING, &opt_pack_size, "Maximum uncompressed size of packfiles in bytes; may be suffixed with k, m, or g", "BYTES" },
  { "compression", 0, 0, G_OPTION_ARG_STRING, &opt_compression, "Compress generated packfiles using COMPRESSION; may be one of 'gzip', 'xz'", "COMPRESSION" },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  guint n_commits;
  guint n_dirmeta;
  guint n_dirtree;
  guint n_files;
  GPtrArray *objects;
  GArray *object_sizes;
  gboolean had_error;
  GError **error;
} OtRepackData;

typedef struct {
  GOutputStream *out;
  GPtrArray *compressor_argv;
  GPid compress_child_pid;
} OtBuildRepackFile;

static void
compressor_child_setup (gpointer user_data)
{
  int stdout_fd = GPOINTER_TO_INT (user_data);

  dup2 (stdout_fd, 1);
}

static gboolean
build_repack_file_init (OtBuildRepackFile *self,
                        GFile             *destfile,
                        GPtrArray         *compressor_argv,
                        GCancellable      *cancellable,
                        GError           **error)
{
  gboolean ret = FALSE;
  int stdin_pipe_fd;
  int target_stdout_fd;

  target_stdout_fd = open (ot_gfile_get_path_cached (destfile), O_WRONLY | O_CREAT);
  if (target_stdout_fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!g_spawn_async_with_pipes (NULL, (char**)compressor_argv->pdata, NULL,
                                 G_SPAWN_SEARCH_PATH, NULL, NULL,
                                 &self->compress_child_pid, &stdin_pipe_fd,
                                 compressor_child_setup, GINT_TO_POINTER (target_stdout_fd), error))
    goto out;
  
  (void) close (target_stdout_fd);

  self->out = g_unix_output_stream_new (stdin_pipe_fd, TRUE);

  ret = TRUE;
 out:
  return ret;
}

static inline int
size_to_bucket (guint64 objsize)
{
  int off;
  if (objsize < 128)
    return 0;

  objsize >>= 7;
  off = 0;
  while (objsize > 0)
    {
      off++;
      objsize >>= 1;
    }
  if (off > 23)
    return 23;
  return off;
}

static void
object_iter_callback (OstreeRepo    *repo,
                      const char    *checksum,
                      OstreeObjectType objtype,
                      GFile         *objf,
                      GFileInfo     *file_info,
                      gpointer       user_data)
{
  gboolean ret = FALSE;
  OtRepackData *data = user_data;
  char *key;
  GError **error = data->error;
  guint64 objsize;
  int bucket;
  GVariant *archive_meta = NULL;
  GFileInfo *archive_info = NULL;

  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_COMMIT:
      data->n_commits++;
      break;
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      data->n_dirtree++;
      break;
    case OSTREE_OBJECT_TYPE_DIR_META:
      data->n_dirmeta++;
      break;
    case OSTREE_OBJECT_TYPE_RAW_FILE:
    case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT:
      data->n_files++;
      break;
    case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META:
      /* Counted under files */
      break;
    }

  objsize = g_file_info_get_size (file_info);

  g_ptr_array_add (data->objects, 

  bucket = size_to_bucket (objsize);
  data->object_bucket_count[bucket]++;
  data->object_bucket_size[bucket] += objsize;
  
  ret = TRUE;
  /* out: */
  ot_clear_gvariant (&archive_meta);
  g_clear_object (&archive_info);
  data->had_error = !ret;
}


gboolean
ostree_builtin_repack (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  OtRepackData data;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  GCancellable *cancellable = NULL;
  int i;
  guint64 total_size;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Recompress objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  data.repo = repo;
  data.error = error;
  data.objects = g_ptr_array_new_with_free_func (g_free);
  data.object_sizes = g_array_new (FALSE, FALSE, sizeof (guint64));

  if (!ostree_repo_iter_objects (repo, object_iter_callback, &data, error))
    goto out;

  if (data.had_error)
    goto out;

  g_print ("Commits: %u\n", data.n_commits);
  g_print ("Tree contents: %u\n", data.n_dirtree);
  g_print ("Tree meta: %u\n", data.n_dirmeta);
  g_print ("Files: %u\n", data.n_files);

  total_size = 0;
  for (i = 0; i < G_N_ELEMENTS(data.object_bucket_size); i++)
    {
      int size;
      if (i == 0)
        size = 128;
      else
        size = 1 << (i + 7);
      g_print ("%d: %" G_GUINT64_FORMAT " objects, %" G_GUINT64_FORMAT " bytes\n",
               size, data.object_bucket_count[i], data.object_bucket_size[i]);
      total_size += data.object_bucket_size[i];
    }
  g_print ("Total size: %" G_GUINT64_FORMAT "\n", total_size);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  return ret;
}
