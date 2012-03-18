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

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#define OT_DEFAULT_PACK_SIZE_BYTES (50*1024*1024)
#define OT_GZIP_COMPRESSION_LEVEL (8)

static gboolean opt_analyze_only;
static char* opt_pack_size;
static char* opt_int_compression;
static char* opt_ext_compression;

typedef enum {
  OT_COMPRESSION_NONE,
  OT_COMPRESSION_GZIP,
  OT_COMPRESSION_XZ
} OtCompressionType;

static GOptionEntry options[] = {
  { "pack-size", 0, 0, G_OPTION_ARG_STRING, &opt_pack_size, "Maximum uncompressed size of packfiles in bytes; may be suffixed with k, m, or g", "BYTES" },
  { "internal-compression", 0, 0, G_OPTION_ARG_STRING, &opt_int_compression, "Compress objects using COMPRESSION", "COMPRESSION" },
  { "external-compression", 0, 0, G_OPTION_ARG_STRING, &opt_ext_compression, "Compress entire packfiles using COMPRESSION", "COMPRESSION" },
  { "analyze-only", 0, 0, G_OPTION_ARG_NONE, &opt_analyze_only, "Just analyze current state", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;

  guint64 pack_size;
  OtCompressionType int_compression;
  OtCompressionType ext_compression;

  guint n_commits;
  guint n_dirmeta;
  guint n_dirtree;
  guint n_files;
  GPtrArray *objects;
  gboolean had_error;
  GError **error;
} OtRepackData;

typedef struct {
  GOutputStream *out;
  GPtrArray *compressor_argv;
  GPid compress_child_pid;
} OtBuildRepackFile;

static GPtrArray *
get_xz_args (void)
{
  GPtrArray *ret = g_ptr_array_new ();
  
  g_ptr_array_add (ret, "xz");
  g_ptr_array_add (ret, "--memlimit-compress=512M");

  return ret;
}

static void
compressor_child_setup (gpointer user_data)
{
  int stdout_fd = GPOINTER_TO_INT (user_data);

  if (dup2 (stdout_fd, 1) < 0)
    g_assert_not_reached ();
  (void) close (stdout_fd);
}

static gboolean
create_compressor_subprocess (OtBuildRepackFile *self,
                              GPtrArray         *compressor_argv,
                              GFile             *destfile,
                              GOutputStream    **out_output,
                              GCancellable      *cancellable,
                              GError           **error)
{
  gboolean ret = FALSE;
  GOutputStream *ret_output = NULL;
  int stdin_pipe_fd;
  int target_stdout_fd;

  target_stdout_fd = open (ot_gfile_get_path_cached (destfile), O_WRONLY);
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

  ret_output = g_unix_output_stream_new (stdin_pipe_fd, TRUE);

  ret = TRUE;
  ot_transfer_out_value (out_output, &ret_output);
 out:
  g_clear_object (&ret_output);
  return ret;
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
  guint64 objsize;
  GVariant *objdata = NULL;

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

  /* For archived content, only count regular files */
  if (!(objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT
        && g_file_info_get_file_type (file_info) != G_FILE_TYPE_REGULAR))
    {
      objsize = g_file_info_get_size (file_info);

      objdata = g_variant_new ("(tsu)", objsize, checksum, (guint32)objtype);
      g_ptr_array_add (data->objects, g_variant_ref_sink (objdata));
      objdata = NULL; /* Transfer ownership */
    }

  ret = TRUE;
  /* out: */
  ot_clear_gvariant (&objdata);
  data->had_error = !ret;
}

static gint
compare_object_data_by_size (gconstpointer    ap,
                             gconstpointer    bp)
{
  GVariant *a = *(void **)ap;
  GVariant *b = *(void **)bp;
  guint64 a_size;
  guint64 b_size;

  g_variant_get_child (a, 0, "t", &a_size);
  g_variant_get_child (b, 0, "t", &b_size);
  if (a == b)
    return 0;
  else if (a > b)
    return 1;
  else
    return -1;
}

static gboolean
write_aligned_variant (GOutputStream      *output,
                       GVariant           *variant,
                       GChecksum          *checksum,
                       guint64            *inout_offset,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  guint padding;
  gsize bytes_written;
  char padding_nuls[7] = {0, 0, 0, 0, 0, 0, 0};

  padding = 8 - ((*inout_offset) & 7);
  
  if (padding > 0)
    {
      g_checksum_update (checksum, (guchar*) padding_nuls, padding);
      if (!g_output_stream_write_all (output, padding_nuls, padding, &bytes_written,
                                      cancellable, error))
        goto out;
      g_assert (bytes_written == padding);
      *inout_offset += padding;
    }

  g_checksum_update (checksum, (guchar*) g_variant_get_data (variant),
                     g_variant_get_size (variant));
  if (!g_output_stream_write_all (output, g_variant_get_data (variant),
                                  g_variant_get_size (variant), &bytes_written,
                                  cancellable, error))
    goto out;
  *inout_offset += bytes_written;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
create_pack_file (OtRepackData        *data,
                  GPtrArray           *objects,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  GFile *pack_dir = NULL;
  GFile *index_temppath = NULL;
  GOutputStream *index_out = NULL;
  GFile *pack_temppath = NULL;
  GOutputStream *pack_out = NULL;
  GFile *object_path = NULL;
  GFileInfo *object_file_info = NULL;
  GFileInputStream *object_input = NULL;
  GConverter *compressor = NULL;
  GConverterOutputStream *compressed_object_output = NULL;
  guint i;
  guint64 offset;
  gsize bytes_read;
  gsize bytes_written;
  GVariantBuilder index_content_builder;
  gboolean index_content_builder_initialized = FALSE;
  GVariant *pack_header = NULL;
  GVariant *object_header = NULL;
  GVariant *index_content = NULL;
  GChecksum *pack_checksum = NULL;
  char *pack_name = NULL;
  GFile *pack_file_path = NULL;
  GFile *pack_index_path = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (data->repo),
                                        "pack-index", NULL,
                                        &index_temppath,
                                        &index_out,
                                        cancellable, error))
    goto out;
  
  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (data->repo),
                                        "pack-content", NULL,
                                        &pack_temppath,
                                        &pack_out,
                                        cancellable, error))
    goto out;

  offset = 0;
  pack_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_variant_builder_init (&index_content_builder, G_VARIANT_TYPE ("a(st)"));
  index_content_builder_initialized = TRUE;

  pack_header = g_variant_new ("(su@a{sv}u)",
                               "OSTPACK", GUINT32_TO_BE (0),
                               g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                               objects->len);

  if (!write_aligned_variant (pack_out, pack_header, pack_checksum, &offset,
                              cancellable, error))
    goto out;
  
  for (i = 0; i < objects->len; i++)
    {
      GVariant *object_data = objects->pdata[i];
      guint64 objsize;
      const char *checksum;
      guint32 objtype_u32;
      OstreeObjectType objtype;
      char buf[4096];
      guint64 obj_bytes_written;
      GOutputStream *write_pack_out;
      guchar entry_flags;

      g_variant_get (object_data, "(t&su)", &objsize, &checksum, &objtype_u32);
                     
      objtype = (OstreeObjectType) objtype_u32;

      ot_clear_gvariant (&object_header);
      entry_flags = 0;
      if (data->int_compression != OT_COMPRESSION_NONE)
        {
          switch (data->int_compression)
            {
          case OT_COMPRESSION_GZIP:
            entry_flags |= OSTREE_PACK_FILE_ENTRY_FLAG_COMPRESSION_GZIP;
            break;
            case OT_COMPRESSION_NONE:
              break;
            default:
              g_assert_not_reached ();
            }
        }

      object_header = g_variant_new ("(yst)", GUINT32_TO_BE (entry_flags), checksum, (guint32)objtype, objsize);

      if (!write_aligned_variant (pack_out, object_header, pack_checksum,
                                  &offset, cancellable, error))
        goto out;
      
      g_clear_object (&object_path);
      object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);
      
      g_clear_object (&object_input);
      object_input = g_file_read (object_path, cancellable, error);
      if (!object_input)
        goto out;

      if (data->int_compression != OT_COMPRESSION_NONE)
        {
          g_clear_object (&compressor);
          switch (data->int_compression)
            {
            case OT_COMPRESSION_GZIP:
              compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, OT_GZIP_COMPRESSION_LEVEL);
              break;
            default:
              g_assert_not_reached ();
            }
          
          g_clear_object (&compressed_object_output);
          compressed_object_output = (GConverterOutputStream*)g_object_new (G_TYPE_CONVERTER_OUTPUT_STREAM,
                                                                            "converter", compressor,
                                                                            "base-stream", pack_out,
                                                                            "close-base-stream", FALSE,
                                                                            NULL);
          write_pack_out = (GOutputStream*)compressed_object_output;
        }
      else
         write_pack_out = (GOutputStream*)pack_out;

      obj_bytes_written = 0;
      do
        {
          if (!g_input_stream_read_all ((GInputStream*)object_input, buf, sizeof(buf), &bytes_read, cancellable, error))
            goto out;
          g_checksum_update (pack_checksum, (guint8*)buf, bytes_read);
          if (bytes_read > 0)
            {
              if (!g_output_stream_write_all (write_pack_out, buf, bytes_read, &bytes_written, cancellable, error))
                goto out;
              offset += bytes_written;
              obj_bytes_written += bytes_written;
            }
        }
      while (bytes_read > 0);

      if (compressed_object_output)
        {
          if (!g_output_stream_flush ((GOutputStream*)compressed_object_output, cancellable, error))
            goto out;
        }

      g_assert_cmpint (obj_bytes_written, ==, objsize);

      g_variant_builder_add (&index_content_builder, "(st)", checksum, offset);
    }
  
  if (!g_output_stream_close (pack_out, cancellable, error))
    goto out;

  pack_dir = g_file_resolve_relative_path (ostree_repo_get_path (data->repo),
                                           "objects/pack");
  if (!ot_gfile_ensure_directory (pack_dir, FALSE, error))
    goto out;

  pack_name = g_strconcat ("ostpack-", g_checksum_get_string (pack_checksum), ".data", NULL);
  pack_file_path = g_file_get_child (pack_dir, pack_name);

  if (rename (ot_gfile_get_path_cached (pack_temppath),
              ot_gfile_get_path_cached (pack_file_path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      g_prefix_error (error, "Failed to rename pack file '%s' to '%s': ",
                      ot_gfile_get_path_cached (pack_temppath),
                      ot_gfile_get_path_cached (pack_file_path));
      goto out;
    }
  g_clear_object (&pack_temppath);

  index_content = g_variant_new ("(su@a{sv}@a(st))",
                                 "OSTPACKINDEX", GUINT32_TO_BE(0),
                                 g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                                 g_variant_builder_end (&index_content_builder));
  index_content_builder_initialized = FALSE;

  if (!g_output_stream_write_all (index_out,
                                  g_variant_get_data (index_content),
                                  g_variant_get_size (index_content),
                                  &bytes_written,
                                  cancellable,
                                  error))
    goto out;

  if (!g_output_stream_close (index_out, cancellable, error))
    goto out;

  g_free (pack_name);
  pack_name = g_strconcat ("ostpack-", g_checksum_get_string (pack_checksum), ".index", NULL);
  pack_index_path = g_file_get_child (pack_dir, pack_name);

  if (rename (ot_gfile_get_path_cached (index_temppath),
              ot_gfile_get_path_cached (pack_index_path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      g_prefix_error (error, "Failed to rename pack file '%s' to '%s': ",
                      ot_gfile_get_path_cached (index_temppath),
                      ot_gfile_get_path_cached (pack_index_path));
      goto out;
    }
  g_clear_object (&index_temppath);

  ret = TRUE;
 out:
  if (index_temppath)
    (void) unlink (ot_gfile_get_path_cached (index_temppath));
  g_clear_object (&index_temppath);
  g_clear_object (&index_out);
  if (pack_temppath)
    (void) unlink (ot_gfile_get_path_cached (pack_temppath));
  g_clear_object (&pack_temppath);
  g_clear_object (&pack_out);
  g_clear_object (&object_path);
  g_clear_object (&object_input);
  g_clear_object (&compressor);
  g_clear_object (&compressed_object_output);
  g_clear_object (&object_file_info);
  ot_clear_gvariant (&object_header);
  if (pack_checksum)
    g_checksum_free (pack_checksum);
  g_clear_object (&pack_dir);
  ot_clear_gvariant (&index_content);
  g_free (pack_name);
  g_clear_object (&pack_file_path);
  g_clear_object (&pack_index_path);
  if (index_content_builder_initialized)
    g_variant_builder_clear (&index_content_builder);
  return ret;
}

/**
 * cluster_objects_stupidly:
 *
 * Just sorts by size currently.
 *
 * Returns: [Array of [Array of object data]].  Free with g_ptr_array_unref().
 */
static GPtrArray *
cluster_objects_stupidly (OtRepackData      *data)
{
  GPtrArray *ret = NULL;
  GPtrArray *objects = data->objects;
  guint i;
  guint64 current_size;
  guint current_offset;

  g_ptr_array_sort (data->objects, compare_object_data_by_size);

  ret = g_ptr_array_new ();

  current_size = 0;
  current_offset = 0;
  for (i = 0; i < objects->len; i++)
    { 
      GVariant *objdata = objects->pdata[i];
      guint64 objsize;

      g_variant_get_child (objdata, 0, "t", &objsize);

      if (current_size + objsize > data->pack_size)
        {
          guint j;
          GPtrArray *current = g_ptr_array_new ();
          for (j = current_offset; j < i; j++)
            {
              g_ptr_array_add (current, objects->pdata[j]);
            }
          g_ptr_array_add (ret, current);
          current_size = objsize;
          current_offset = i;
        }
      else if (objsize > data->pack_size)
        {
          break;
        }
      else
        {
          current_size += objsize;
        }
    }

  return ret;
}

static gboolean
parse_size_spec_with_suffix (const char *spec,
                             guint64     default_value,
                             guint64    *out_size,
                             GError    **error)
{
  gboolean ret = FALSE;
  char *endptr = NULL;
  guint64 ret_size;

  if (spec == NULL)
    {
      ret_size = default_value;
      endptr = NULL;
    }
  else
    {
      ret_size = g_ascii_strtoull (spec, &endptr, 10);
  
      if (endptr && *endptr)
        {
          char suffix = *endptr;
      
          switch (suffix)
            {
            case 'k':
            case 'K':
              {
                ret_size *= 1024;
                break;
              }
            case 'm':
            case 'M':
              {
                ret_size *= (1024 * 1024);
                break;
              }
            case 'g':
            case 'G':
              {
                ret_size *= (1024 * 1024 * 1024);
                break;
              }
            default:
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid size suffix '%c'", suffix);
              goto out;
            }
        }
    }

  ret = TRUE;
  *out_size = ret_size;
 out:
  return ret;
}

static gboolean
parse_compression_string (const char *compstr,
                          OtCompressionType *out_comptype,
                          GError           **error)
{
  gboolean ret = FALSE;
  OtCompressionType ret_comptype;
  
  if (compstr == NULL)
    ret_comptype = OT_COMPRESSION_NONE;
  else if (strcmp (compstr, "gzip") == 0)
    ret_comptype = OT_COMPRESSION_GZIP;
  else if (strcmp (compstr, "xz") == 0)
    ret_comptype = OT_COMPRESSION_XZ;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid compression '%s'", compstr);
      goto out;
    }

  ret = TRUE;
  *out_comptype = ret_comptype;
 out:
  return ret;
}

gboolean
ostree_builtin_repack (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  OtRepackData data;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  GCancellable *cancellable = NULL;
  guint i;
  guint64 total_size;
  GPtrArray *clusters = NULL;

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
  data.objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  if (!parse_size_spec_with_suffix (opt_pack_size, OT_DEFAULT_PACK_SIZE_BYTES, &data.pack_size, error))
    goto out;
  /* Default internal compression to gzip */
  if (!parse_compression_string (opt_int_compression ? opt_int_compression : "gzip", &data.int_compression, error))
    goto out;
  if (!parse_compression_string (opt_ext_compression, &data.ext_compression, error))
    goto out;

  if (!ostree_repo_iter_objects (repo, object_iter_callback, &data, error))
    goto out;

  if (data.had_error)
    goto out;

  g_print ("Commits: %u\n", data.n_commits);
  g_print ("Tree contents: %u\n", data.n_dirtree);
  g_print ("Tree meta: %u\n", data.n_dirmeta);
  g_print ("Files: %u\n", data.n_files);

  total_size = 0;
  for (i = 0; i < data.objects->len; i++)
    {
      GVariant *objdata = data.objects->pdata[i];
      guint64 size;
      
      g_variant_get_child (objdata, 0, "t", &size);
      
      total_size += size;
    }
  g_print ("Total size: %" G_GUINT64_FORMAT "\n", total_size);

  g_print ("\n");
  g_print ("Using pack size: %" G_GUINT64_FORMAT "\n", data.pack_size);

  clusters = cluster_objects_stupidly (&data);

  g_print ("Going to create %u packfiles\n", clusters->len);

  for (i = 0; i < clusters->len; i++)
    {
      GPtrArray *cluster = clusters->pdata[i];
      
      g_print ("%u: %u objects\n", i, cluster->len);

      if (!opt_analyze_only)
        {
          if (!create_pack_file (&data, cluster, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (clusters)
    g_ptr_array_unref (clusters);
  return ret;
}
