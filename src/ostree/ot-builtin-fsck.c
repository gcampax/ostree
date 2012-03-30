/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

static gboolean quiet;
static gboolean delete;

static GOptionEntry options[] = {
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Don't display informational messages", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &delete, "Remove corrupted objects", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  guint n_objects;
  gboolean had_error;
} OtFsckData;

static gboolean
checksum_archived_file (OtFsckData   *data,
                        const char   *exp_checksum,
                        GFile        *file,
                        GChecksum   **out_checksum,
                        GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *archive_metadata = NULL;
  GVariant *xattrs = NULL;
  GFile *content_path = NULL;
  GInputStream *content_input = NULL;
  GFileInfo *file_info = NULL;
  char buf[8192];
  gsize bytes_read;
  guint32 mode;

  if (!ostree_map_metadata_file (file, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META, &archive_metadata, error))
    goto out;

  if (!ostree_parse_archived_file_meta (archive_metadata, &file_info, &xattrs, error))
    goto out;

  content_path = ostree_repo_get_object_path (data->repo, exp_checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      content_input = (GInputStream*)g_file_read (content_path, NULL, error);
      if (!content_input)
        goto out;
    }

  ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  if (S_ISREG (mode))
    {
      g_assert (content_input != NULL);
      do
        {
          if (!g_input_stream_read_all (content_input, buf, sizeof(buf), &bytes_read, NULL, error))
            goto out;
          g_checksum_update (ret_checksum, (guint8*)buf, bytes_read);
        }
      while (bytes_read > 0);
    }
  else if (S_ISLNK (mode))
    {
      const char *target = g_file_info_get_attribute_byte_string (file_info, "standard::symlink-target");
      g_checksum_update (ret_checksum, (guint8*) target, strlen (target));
    }
  else if (S_ISBLK (mode) || S_ISCHR (mode))
    {
      guint32 rdev = g_file_info_get_attribute_uint32 (file_info, "unix::rdev");
      guint32 rdev_be;
      
      rdev_be = GUINT32_TO_BE (rdev);

      g_checksum_update (ret_checksum, (guint8*)&rdev_be, 4);
    }

  ostree_checksum_update_stat (ret_checksum,
                               g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                               g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                               mode);
  if (xattrs)
    g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));

  ret = TRUE;
  ot_transfer_out_value (out_checksum, &ret_checksum);
 out:
  ot_clear_checksum (&ret_checksum);
  g_clear_object (&file_info);
  ot_clear_gvariant (&xattrs);
  ot_clear_gvariant (&archive_metadata);
  g_clear_object (&content_path);
  g_clear_object (&content_input);
  return ret;
}

static void
encountered_fsck_error (OtFsckData  *data,
                        const char  *fmt,
                        ...) G_GNUC_PRINTF (2, 3);

static void
encountered_fsck_error (OtFsckData  *data,
                        const char  *fmt,
                        ...)
{
  va_list args;
  char *msg;

  va_start (args, fmt);

  g_vasprintf (&msg, fmt, args);
  g_printerr ("ERROR: %s\n", msg);
  data->had_error = TRUE;

  va_end (args);
}
                        

static gboolean
fsck_loose_object (OtFsckData    *data,
                   const char    *exp_checksum,
                   OstreeObjectType objtype,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  GFile *objf = NULL;
  GChecksum *real_checksum = NULL;

  objf = ostree_repo_get_object_path (data->repo, exp_checksum, objtype);

  if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META)
    {
      if (!g_str_has_suffix (ot_gfile_get_path_cached (objf), ".archive-meta"))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid archive filename '%s'",
                       ot_gfile_get_path_cached (objf));
          goto out;
        }
      if (!checksum_archived_file (data, exp_checksum, objf, &real_checksum, error))
        goto out;
    }
  else if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
    ; /* Handled above */
  else
    {
      if (!ostree_checksum_file (objf, objtype, &real_checksum, NULL, error))
        goto out;
    }

  if (real_checksum && strcmp (exp_checksum, g_checksum_get_string (real_checksum)) != 0)
    {

      encountered_fsck_error (data, "corrupted object '%s'; actual checksum: %s",
                              ot_gfile_get_path_cached (objf), g_checksum_get_string (real_checksum));
      if (delete)
        (void) unlink (ot_gfile_get_path_cached (objf));
    }

  data->n_objects++;

  ret = TRUE;
 out:
  ot_clear_checksum (&real_checksum);
  return ret;
}

static gboolean
fsck_pack_files (OtFsckData  *data,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *pack_indexes = NULL;
  GVariant *index_variant = NULL;
  GFile *pack_index_path = NULL;
  GFile *pack_data_path = NULL;
  GInputStream *input = NULL;
  GChecksum *pack_content_checksum = NULL;
  guint i;

  if (!ostree_repo_list_pack_indexes (data->repo, &pack_indexes, cancellable, error))
    goto out;

  for (i = 0; i < pack_indexes->len; i++)
    {
      const char *checksum = pack_indexes->pdata[i];

      g_clear_object (&pack_index_path);
      pack_index_path = ostree_repo_get_pack_index_path (data->repo, checksum);

      ot_clear_gvariant (&index_variant);
      if (!ot_util_variant_map (pack_index_path,
                                OSTREE_PACK_INDEX_VARIANT_FORMAT,
                                &index_variant, error))
        goto out;
      
      if (!ostree_validate_structureof_pack_index (index_variant, error))
        {
          g_prefix_error (error, "Corrupted pack index '%s': ",
                          ot_gfile_get_path_cached (pack_index_path));
          goto out;
        }

      g_clear_object (&pack_data_path);
      pack_data_path = ostree_repo_get_pack_data_path (data->repo, checksum);
      
      g_clear_object (&input);
      input = (GInputStream*)g_file_read (pack_data_path, cancellable, error);
      if (!input)
        goto out;
     
      if (pack_content_checksum)
        g_checksum_free (pack_content_checksum);
      if (!ot_gio_checksum_stream (input, &pack_content_checksum, cancellable, error))
        goto out;

      if (strcmp (g_checksum_get_string (pack_content_checksum), checksum) != 0)
        {
          encountered_fsck_error (data, "corrupted pack '%s', expected checksum %s",
                                  checksum, g_checksum_get_string (pack_content_checksum));
        }
    }

  ret = TRUE;
 out:
  if (pack_content_checksum)
    g_checksum_free (pack_content_checksum);
  if (pack_indexes)
    g_ptr_array_unref (pack_indexes);
  g_clear_object (&pack_data_path);
  g_clear_object (&input);
  return ret;
}


gboolean
ostree_builtin_fsck (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  OtFsckData data;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  GHashTable *objects = NULL;
  GCancellable *cancellable = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;

  context = g_option_context_new ("- Check the repository for consistency");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  data.repo = repo;
  data.n_objects = 0;
  data.had_error = FALSE;

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    goto out;
  
  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_variant_get_child (objdata, 0, "b", &is_loose);

      if (is_loose)
        {
          if (!fsck_loose_object (&data, checksum, objtype, cancellable, error))
            goto out;
        }
    }

  if (!fsck_pack_files (&data, cancellable, error))
    goto out;

  if (data.had_error)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Encountered filesystem consistency errors");
      goto out;
    }
  if (!quiet)
    g_print ("Total Objects: %u\n", data.n_objects);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (objects)
    g_hash_table_unref (objects);
  return ret;
}
