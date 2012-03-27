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
static gboolean opt_keep_loose;
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
  { "keep-loose", 0, 0, G_OPTION_ARG_NONE, &opt_keep_loose, "Don't delete loose objects", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;

  guint64 pack_size;
  OtCompressionType int_compression;
  OtCompressionType ext_compression;

  gboolean had_error;
  GError **error;
} OtRepackData;

typedef struct {
  GOutputStream *out;
  GPtrArray *compressor_argv;
  GPid compress_child_pid;
} OtBuildRepackFile;

static gint
compare_object_data_by_size (gconstpointer    ap,
                             gconstpointer    bp)
{
  GVariant *a = *(void **)ap;
  GVariant *b = *(void **)bp;
  guint64 a_size;
  guint64 b_size;

  g_variant_get_child (a, 2, "t", &a_size);
  g_variant_get_child (b, 2, "t", &b_size);
  if (a == b)
    return 0;
  else if (a > b)
    return 1;
  else
    return -1;
}

static gboolean
write_bytes_update_checksum (GOutputStream *output,
                             gconstpointer  bytes,
                             gsize          len,
                             GChecksum     *checksum,
                             guint64       *inout_offset,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;

  if (len > 0)
    {
      g_checksum_update (checksum, (guchar*) bytes, len);
      if (!g_output_stream_write_all (output, bytes, len, &bytes_written,
                                      cancellable, error))
        goto out;
      g_assert_cmpint (bytes_written, ==, len);
      *inout_offset += bytes_written;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_padding (GOutputStream    *output,
               guint             alignment,
               GChecksum        *checksum,
               guint64          *inout_offset,
               GCancellable     *cancellable,
               GError          **error)
{
  gboolean ret = FALSE;
  guint bits;
  guint padding_len;
  guchar padding_nuls[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (alignment == 8)
    bits = ((*inout_offset) & 7);
  else
    bits = ((*inout_offset) & 3);

  if (bits > 0)
    {
      padding_len = alignment - bits;
      if (!write_bytes_update_checksum (output, (guchar*)padding_nuls, padding_len,
                                        checksum, inout_offset, cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_variant_with_size (GOutputStream      *output,
                         GVariant           *variant,
                         GChecksum          *checksum,
                         guint64            *inout_offset,
                         GCancellable       *cancellable,
                         GError            **error)
{
  gboolean ret = FALSE;
  guint64 variant_size;
  guint32 variant_size_u32_be;

  g_assert ((*inout_offset & 3) == 0);

  /* Write variant size */
  variant_size = g_variant_get_size (variant);
  g_assert (variant_size < G_MAXUINT32);
  variant_size_u32_be = GUINT32_TO_BE((guint32) variant_size);

  if (!write_bytes_update_checksum (output, (guchar*)&variant_size_u32_be, 4,
                                    checksum, inout_offset, cancellable, error))
    goto out;

  /* Pad to offset of 8, write variant */
  if (!write_padding (output, 8, checksum, inout_offset, cancellable, error))
    goto out;
  g_assert ((*inout_offset & 7) == 0);

  if (!write_bytes_update_checksum (output, g_variant_get_data (variant),
                                    variant_size, checksum,
                                    inout_offset, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gint
compare_index_content (gconstpointer         ap,
                       gconstpointer         bp)
{
  gpointer a = *((gpointer*)ap);
  gpointer b = *((gpointer*)bp);
  GVariant *a_v = a;
  GVariant *b_v = b;
  GVariant *a_csum_bytes;
  GVariant *b_csum_bytes;
  guint32 a_objtype;
  guint32 b_objtype;
  guint64 a_offset;
  guint64 b_offset;
  int c;

  g_variant_get (a_v, "(u@ayt)", &a_objtype, &a_csum_bytes, &a_offset);      
  g_variant_get (b_v, "(u@ayt)", &b_objtype, &b_csum_bytes, &b_offset);      
  a_objtype = GUINT32_FROM_BE (a_objtype);
  b_objtype = GUINT32_FROM_BE (b_objtype);
  c = ostree_cmp_checksum_bytes (a_csum_bytes, b_csum_bytes);
  if (c == 0)
    {
      if (a_objtype < b_objtype)
        c = -1;
      else if (a_objtype > b_objtype)
        c = 1;
    }
  return c;
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
  GConverterInputStream *compressed_object_input = NULL;
  guint i;
  guint64 offset;
  gsize bytes_read;
  gsize bytes_written;
  GPtrArray *index_content_list = NULL;
  GVariant *pack_header = NULL;
  GVariant *packed_object = NULL;
  GVariant *index_content = NULL;
  GVariantBuilder index_content_builder;
  GChecksum *pack_checksum = NULL;
  char *pack_name = NULL;
  GFile *pack_file_path = NULL;
  GFile *pack_index_path = NULL;
  GMemoryOutputStream *object_data_stream = NULL;

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

  index_content_list = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  offset = 0;
  pack_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  pack_header = g_variant_new ("(s@a{sv}t)",
                               "OSTv0PACKFILE",
                               g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                               (guint64)objects->len);

  if (!write_variant_with_size (pack_out, pack_header, pack_checksum, &offset,
                                cancellable, error))
    goto out;
  
  for (i = 0; i < objects->len; i++)
    {
      GVariant *object_data = objects->pdata[i];
      const char *checksum;
      guint32 objtype_u32;
      OstreeObjectType objtype;
      char buf[4096];
      guint64 expected_objsize;
      guint64 objsize;
      GInputStream *read_object_in;
      guchar entry_flags = 0;
      GVariant *index_entry;

      g_variant_get (object_data, "(&sut)", &checksum, &objtype_u32, &expected_objsize);
                     
      objtype = (OstreeObjectType) objtype_u32;

      switch (data->int_compression)
        {
        case OT_COMPRESSION_GZIP:
          {
            entry_flags |= OSTREE_PACK_FILE_ENTRY_FLAG_GZIP;
            break;
          }
        default:
          {
            g_assert_not_reached ();
          }
        }

      g_clear_object (&object_path);
      object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);
      
      g_clear_object (&object_input);
      object_input = g_file_read (object_path, cancellable, error);
      if (!object_input)
        goto out;

      g_clear_object (&object_file_info);
      object_file_info = g_file_input_stream_query_info (object_input, OSTREE_GIO_FAST_QUERYINFO, cancellable, error);
      if (!object_file_info)
        goto out;

      objsize = g_file_info_get_attribute_uint64 (object_file_info, G_FILE_ATTRIBUTE_STANDARD_SIZE);

      g_assert_cmpint (objsize, ==, expected_objsize);

      g_clear_object (&object_data_stream);
      object_data_stream = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
      
      if (entry_flags & OSTREE_PACK_FILE_ENTRY_FLAG_GZIP)
        {
          g_clear_object (&compressor);
          compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, OT_GZIP_COMPRESSION_LEVEL);
          
          g_clear_object (&compressed_object_input);
          compressed_object_input = (GConverterInputStream*)g_object_new (G_TYPE_CONVERTER_INPUT_STREAM,
                                                                          "converter", compressor,
                                                                          "base-stream", object_input,
                                                                          "close-base-stream", TRUE,
                                                                          NULL);
          read_object_in = (GInputStream*)compressed_object_input;
        }
      else
        {
          read_object_in = (GInputStream*)object_input;
        }

      do
        {
          if (!g_input_stream_read_all (read_object_in, buf, sizeof(buf), &bytes_read, cancellable, error))
            goto out;
          if (!g_output_stream_write_all ((GOutputStream*)object_data_stream, buf, bytes_read, &bytes_written, cancellable, error))
            goto out;
        }
      while (bytes_read > 0);

      if (!g_input_stream_close (read_object_in, cancellable, error))
        goto out;

      ot_clear_gvariant (&packed_object);
      {
        guchar *data = g_memory_output_stream_get_data (object_data_stream);
        gsize data_len = g_memory_output_stream_get_data_size (object_data_stream);
        packed_object = g_variant_new ("(uy@ay@ay)", GUINT32_TO_BE ((guint32)objtype),
                                       entry_flags,
                                       ostree_checksum_to_bytes (checksum),
                                       g_variant_new_fixed_array (G_VARIANT_TYPE ("y"),
                                                                  data, data_len,
                                                                  1));
        g_clear_object (&object_data_stream);
      }

      if (!write_padding (pack_out, 4, pack_checksum, &offset, cancellable, error))
        goto out;

      /* offset points to aligned header size */
      index_entry = g_variant_new ("(u@ayt)",
                                   GUINT32_TO_BE ((guint32)objtype),
                                   ostree_checksum_to_bytes (checksum),
                                   GUINT64_TO_BE (offset));
      g_ptr_array_add (index_content_list, g_variant_ref_sink (index_entry));

      if (!write_variant_with_size (pack_out, packed_object, pack_checksum,
                                    &offset, cancellable, error))
        goto out;
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

  g_variant_builder_init (&index_content_builder, G_VARIANT_TYPE ("a(uayt)"));
  g_ptr_array_sort (index_content_list, compare_index_content);
  for (i = 0; i < index_content_list->len; i++)
    {
      GVariant *index_item = index_content_list->pdata[i];
      g_variant_builder_add_value (&index_content_builder, index_item);
    }
  index_content = g_variant_new ("(s@a{sv}@a(uayt))",
                                 "OSTv0PACKINDEX",
                                 g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                                 g_variant_builder_end (&index_content_builder));

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
  g_clear_object (&compressed_object_input);
  g_clear_object (&object_file_info);
  if (pack_checksum)
    g_checksum_free (pack_checksum);
  g_clear_object (&pack_dir);
  ot_clear_gvariant (&index_content);
  g_free (pack_name);
  g_clear_object (&pack_file_path);
  g_clear_object (&pack_index_path);
  if (index_content_list)
    g_ptr_array_unref (index_content_list);
  return ret;
}

/**
 * cluster_objects_stupidly:
 * @objects: Map from serialized object name to objdata
 * @out_clusters: (out): [Array of [Array of object data]].  Free with g_ptr_array_unref().
 *
 * Just sorts by size currently.  Also filters out non-regular object
 * content.
 */
static gboolean
cluster_objects_stupidly (OtRepackData      *data,
                          GHashTable        *objects,
                          GPtrArray        **out_clusters,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  GPtrArray *ret_clusters = NULL;
  GPtrArray *object_list = NULL;
  guint i;
  guint64 current_size;
  guint current_offset;
  GHashTableIter hash_iter;
  gpointer key, value;
  GFile *object_path = NULL;
  GFileInfo *object_info = NULL;

  object_list = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      guint64 size;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_clear_object (&object_path);
      object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);

      g_clear_object (&object_info);
      object_info = g_file_query_info (object_path, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
      if (!object_info)
        goto out;

      if (g_file_info_get_file_type (object_info) != G_FILE_TYPE_REGULAR)
        continue;

      size = g_file_info_get_attribute_uint64 (object_info, G_FILE_ATTRIBUTE_STANDARD_SIZE);

      g_ptr_array_add (object_list,
                       g_variant_ref_sink (g_variant_new ("(sut)", checksum, (guint32)objtype, size)));
    }

  g_ptr_array_sort (object_list, compare_object_data_by_size);

  ret_clusters = g_ptr_array_new_with_free_func ((GDestroyNotify)g_ptr_array_unref);

  current_size = 0;
  current_offset = 0;
  for (i = 0; i < object_list->len; i++)
    { 
      GVariant *objdata = object_list->pdata[i];
      guint64 objsize;

      g_variant_get_child (objdata, 2, "t", &objsize);

      if (current_size + objsize > data->pack_size || i == (object_list->len - 1))
        {
          guint j;
          GPtrArray *current;

          if (current_offset < i)
            {
              current = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
              for (j = current_offset; j < i; j++)
                {
                  g_ptr_array_add (current, g_variant_ref (object_list->pdata[j]));
                }
              g_ptr_array_add (ret_clusters, current);
              current_size = objsize;
              current_offset = i;
            }
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

  ret = TRUE;
  ot_transfer_out_value (out_clusters, &ret_clusters);
 out:
  if (object_list)
    g_ptr_array_unref (object_list);
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

static gboolean
do_stats_gather_loose (OtRepackData  *data,
                       GHashTable    *objects,
                       GHashTable   **out_loose,
                       GCancellable  *cancellable,
                       GError       **error)
{
  gboolean ret = FALSE;
  GHashTable *ret_loose = NULL;
  guint n_loose = 0;
  guint n_loose_and_packed = 0;
  guint n_packed = 0;
  guint n_dup_packed = 0;
  guint n_commits = 0;
  guint n_dirmeta = 0;
  guint n_dirtree = 0;
  guint n_files = 0;
  GHashTableIter hash_iter;
  gpointer key, value;

  ret_loose = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                     (GDestroyNotify) g_variant_unref,
                                     NULL);

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      gboolean is_packed;
      GVariant *pack_array;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_variant_get (objdata, "(b@as)", &is_loose, &pack_array);

      is_packed = g_variant_n_children (pack_array) > 0;
      
      if (is_loose && is_packed)
        {
          n_loose_and_packed++;
        }
      else if (is_loose)
        {
          GVariant *copy = g_variant_ref (serialized_key);
          g_hash_table_replace (ret_loose, copy, copy);
          n_loose++;
        }
      else if (g_variant_n_children (pack_array) > 1)
        {
          n_dup_packed++;
        }
      else
        {
          n_packed++;
        }
          
      switch (objtype)
        {
        case OSTREE_OBJECT_TYPE_COMMIT:
          n_commits++;
          break;
        case OSTREE_OBJECT_TYPE_DIR_TREE:
          n_dirtree++;
          break;
        case OSTREE_OBJECT_TYPE_DIR_META:
          n_dirmeta++;
          break;
        case OSTREE_OBJECT_TYPE_RAW_FILE:
        case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META:
          n_files++;
          break;
        case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT:
          /* Counted under files by META */
          break;
        }
    }

  g_print ("Commits: %u\n", n_commits);
  g_print ("Tree contents: %u\n", n_dirtree);
  g_print ("Tree meta: %u\n", n_dirmeta);
  g_print ("Files: %u\n", n_files);
  g_print ("\n");
  g_print ("Loose+packed objects: %u\n", n_loose_and_packed);
  g_print ("Loose-only objects: %u\n", n_loose);
  g_print ("Duplicate packed objects: %u\n", n_dup_packed);
  g_print ("Packed-only objects: %u\n", n_packed);

  ret = TRUE;
  ot_transfer_out_value (out_loose, &ret_loose);
 /* out: */
  if (ret_loose)
    g_hash_table_unref (ret_loose);
  return ret;
}

gboolean
ostree_builtin_repack (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  OtRepackData data;
  OstreeRepo *repo = NULL;
  GHashTable *objects = NULL;
  GCancellable *cancellable = NULL;
  guint i;
  GPtrArray *clusters = NULL;
  GHashTable *loose_objects = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  GFile *objpath = NULL;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Recompress objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (ostree_repo_get_mode (repo) != OSTREE_REPO_MODE_ARCHIVE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Can't repack bare repositories yet");
      goto out;
    }

  data.repo = repo;
  data.error = error;

  if (!parse_size_spec_with_suffix (opt_pack_size, OT_DEFAULT_PACK_SIZE_BYTES, &data.pack_size, error))
    goto out;
  /* Default internal compression to gzip */
  if (!parse_compression_string (opt_int_compression ? opt_int_compression : "gzip", &data.int_compression, error))
    goto out;
  if (!parse_compression_string (opt_ext_compression, &data.ext_compression, error))
    goto out;

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects, cancellable, error))
    goto out;

  if (!do_stats_gather_loose (&data, objects, &loose_objects, cancellable, error))
    goto out;

  g_print ("\n");
  g_print ("Using pack size: %" G_GUINT64_FORMAT "\n", data.pack_size);

  if (!cluster_objects_stupidly (&data, loose_objects, &clusters, cancellable, error))
    goto out;
  
  if (clusters->len > 0)
    g_print ("Going to create %u packfiles\n", clusters->len);
  else
    g_print ("Nothing to do\n");
  
  for (i = 0; i < clusters->len; i++)
    {
      GPtrArray *cluster = clusters->pdata[i];
      
      if (!opt_analyze_only)
        {
          if (!create_pack_file (&data, cluster, cancellable, error))
            goto out;
        }
    }

  if (!opt_analyze_only && !opt_keep_loose)
    {
      g_hash_table_iter_init (&hash_iter, objects);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          GVariant *serialized_key = key;
          GVariant *objdata = value;
          const char *checksum;
          OstreeObjectType objtype;
          gboolean is_loose;
          GVariant *pack_array;

          ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

          g_variant_get (objdata, "(b@as)", &is_loose, &pack_array);

          if (is_loose && g_variant_n_children (pack_array) > 0)
            {
              g_clear_object (&objpath);
              objpath = ostree_repo_get_object_path (data.repo, checksum, objtype);
              if (!ot_gfile_unlink (objpath, cancellable, error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  g_clear_object (&objpath);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (clusters)
    g_ptr_array_unref (clusters);
  if (loose_objects)
    g_hash_table_unref (loose_objects);
  if (objects)
    g_hash_table_unref (objects);
  return ret;
}
