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

#include "ostree.h"
#include "otutil.h"

#include <sys/types.h>
#include <attr/xattr.h>

gboolean
ostree_validate_checksum_string (const char *sha256,
                                 GError    **error)
{
  int i = 0;
  size_t len = strlen (sha256);

  if (len != 64)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev '%s'", sha256);
      return FALSE;
    }

  for (i = 0; i < len; i++)
    {
      guint8 c = ((guint8*) sha256)[i];

      if (!((c >= 48 && c <= 57)
            || (c >= 97 && c <= 102)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid character '%d' in rev '%s'",
                       c, sha256);
          return FALSE;
        }
    }
  return TRUE;
}

gboolean
ostree_validate_rev (const char *rev,
                     GError **error)
{
  gboolean ret = FALSE;
  GPtrArray *components = NULL;

  if (!ot_util_path_split_validate (rev, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty rev");
      goto out;
    }

  ret = TRUE;
 out:
  g_ptr_array_unref (components);
  return ret;
}

GVariant *
ostree_wrap_metadata_variant (OstreeObjectType  type,
                              GVariant         *metadata)
{
  return g_variant_new ("(uv)", GUINT32_TO_BE ((guint32)type), metadata);
}

void
ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode)
{
  guint32 perms;
  perms = GUINT32_TO_BE (mode & ~S_IFMT);
  uid = GUINT32_TO_BE (uid);
  gid = GUINT32_TO_BE (gid);
  g_checksum_update (checksum, (guint8*) &uid, 4);
  g_checksum_update (checksum, (guint8*) &gid, 4);
  g_checksum_update (checksum, (guint8*) &perms, 4);
}

static char *
canonicalize_xattrs (char *xattr_string, size_t len)
{
  char *p;
  GSList *xattrs = NULL;
  GSList *iter;
  GString *result;

  result = g_string_new (0);

  p = xattr_string;
  while (p < xattr_string+len)
    {
      xattrs = g_slist_prepend (xattrs, p);
      p += strlen (p) + 1;
    }

  xattrs = g_slist_sort (xattrs, (GCompareFunc) strcmp);
  for (iter = xattrs; iter; iter = iter->next)
    g_string_append (result, iter->data);

  g_slist_free (xattrs);
  return g_string_free (result, FALSE);
}

static gboolean
read_xattr_name_array (const char *path,
                       const char *xattrs,
                       size_t      len,
                       GVariantBuilder *builder,
                       GError  **error)
{
  gboolean ret = FALSE;
  const char *p;

  p = xattrs;
  while (p < xattrs+len)
    {
      ssize_t bytes_read;
      char *buf;

      bytes_read = lgetxattr (path, p, NULL, 0);
      if (bytes_read < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      if (bytes_read == 0)
        continue;

      buf = g_malloc (bytes_read);
      if (lgetxattr (path, p, buf, bytes_read) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          g_free (buf);
          goto out;
        }
      
      g_variant_builder_add (builder, "(@ay@ay)",
                             g_variant_new_bytestring (p),
                             g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                                      buf, bytes_read, FALSE, g_free, buf));

      p = p + strlen (p) + 1;
    }
  
  ret = TRUE;
 out:
  return ret;
}

GVariant *
ostree_get_xattrs_for_file (GFile      *f,
                            GError    **error)
{
  const char *path;
  GVariant *ret = NULL;
  GVariantBuilder builder;
  char *xattr_names = NULL;
  char *xattr_names_canonical = NULL;
  ssize_t bytes_read;

  path = ot_gfile_get_path_cached (f);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

  bytes_read = llistxattr (path, NULL, 0);

  if (bytes_read < 0)
    {
      if (errno != ENOTSUP)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (bytes_read > 0)
    {
      xattr_names = g_malloc (bytes_read);
      if (llistxattr (path, xattr_names, bytes_read) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      xattr_names_canonical = canonicalize_xattrs (xattr_names, bytes_read);
      
      if (!read_xattr_name_array (path, xattr_names_canonical, bytes_read, &builder, error))
        goto out;
    }

  ret = g_variant_builder_end (&builder);
  g_variant_ref_sink (ret);
 out:
  if (!ret)
    g_variant_builder_clear (&builder);
  g_free (xattr_names);
  g_free (xattr_names_canonical);
  return ret;
}

gboolean
ostree_checksum_file_from_input (GFileInfo        *file_info,
                                 GVariant         *xattrs,
                                 GInputStream     *in,
                                 OstreeObjectType objtype,
                                 GChecksum       **out_checksum,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;
  GVariant *packed = NULL;
  guint8 buf[8192];
  gsize bytes_read;
  guint32 mode;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    return ot_gio_checksum_stream (in, out_checksum, cancellable, error);

  ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      dirmeta = ostree_create_directory_metadata (file_info, xattrs);
      packed = ostree_wrap_metadata_variant (OSTREE_OBJECT_TYPE_DIR_META, dirmeta);
      g_checksum_update (ret_checksum, g_variant_get_data (packed),
                         g_variant_get_size (packed));

    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      while ((bytes_read = g_input_stream_read (in, buf, sizeof (buf), cancellable, error)) > 0)
        g_checksum_update (ret_checksum, buf, bytes_read);
      if (bytes_read < 0)
        goto out;
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      const char *symlink_target = g_file_info_get_symlink_target (file_info);

      g_assert (symlink_target != NULL);
      
      g_checksum_update (ret_checksum, (guint8*)symlink_target, strlen (symlink_target));
    }
  else if (S_ISCHR(mode) || S_ISBLK(mode))
    {
      guint32 rdev = g_file_info_get_attribute_uint32 (file_info, "unix::rdev");
      rdev = GUINT32_TO_BE (rdev);
      g_checksum_update (ret_checksum, (guint8*)&rdev, 4);
    }
  else if (S_ISFIFO(mode))
    {
      ;
    }
  else
    {
      g_set_error (error, G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Unsupported file (must be regular, symbolic link, fifo, or character/block device)");
      goto out;
    }

  if (objtype != OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
    {
      ostree_checksum_update_stat (ret_checksum,
                                   g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
      /* FIXME - ensure empty xattrs are the same as NULL xattrs */
      if (xattrs)
        g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  ret = TRUE;
  ot_transfer_out_value (out_checksum, &ret_checksum);
 out:
  ot_clear_checksum (&ret_checksum);
  ot_clear_gvariant (&dirmeta);
  ot_clear_gvariant (&packed);
  return ret;
}

gboolean
ostree_checksum_file (GFile            *f,
                      OstreeObjectType objtype,
                      GChecksum       **out_checksum,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GChecksum *ret_checksum = NULL;
  GInputStream *in = NULL;
  GVariant *xattrs = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      in = (GInputStream*)g_file_read (f, cancellable, error);
      if (!in)
        goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE)
    {
      xattrs = ostree_get_xattrs_for_file (f, error);
      if (!xattrs)
        goto out;
    }

  if (!ostree_checksum_file_from_input (file_info, xattrs, in, objtype,
                                        &ret_checksum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  g_clear_object (&file_info);
  g_clear_object (&in);
  ot_clear_checksum(&ret_checksum);
  ot_clear_gvariant(&xattrs);
  return ret;
}

typedef struct {
  GFile  *f;
  OstreeObjectType objtype;
  GChecksum *checksum;
} ChecksumFileAsyncData;

static void
checksum_file_async_thread (GSimpleAsyncResult  *res,
                            GObject             *object,
                            GCancellable        *cancellable)
{
  GError *error = NULL;
  ChecksumFileAsyncData *data;
  GChecksum *checksum = NULL;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_checksum_file (data->f, data->objtype, &checksum, cancellable, &error))
    g_simple_async_result_take_error (res, error);
  else
    data->checksum = checksum;
}

static void
checksum_file_async_data_free (gpointer datap)
{
  ChecksumFileAsyncData *data = datap;

  g_object_unref (data->f);
  ot_clear_checksum (&data->checksum);
  g_free (data);
}
  
void
ostree_checksum_file_async (GFile                 *f,
                            OstreeObjectType       objtype,
                            int                    io_priority,
                            GCancellable          *cancellable,
                            GAsyncReadyCallback    callback,
                            gpointer               user_data)
{
  GSimpleAsyncResult  *res;
  ChecksumFileAsyncData *data;

  data = g_new0 (ChecksumFileAsyncData, 1);
  data->f = g_object_ref (f);
  data->objtype = objtype;

  res = g_simple_async_result_new (G_OBJECT (f), callback, user_data, ostree_checksum_file_async);
  g_simple_async_result_set_op_res_gpointer (res, data, (GDestroyNotify)checksum_file_async_data_free);
  
  g_simple_async_result_run_in_thread (res, checksum_file_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

gboolean
ostree_checksum_file_async_finish (GFile          *f,
                                   GAsyncResult   *result,
                                   GChecksum     **out_checksum,
                                   GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  ChecksumFileAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_checksum_file_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_checksum = data->checksum;
  data->checksum = NULL;
  return TRUE;
}

GVariant *
ostree_create_directory_metadata (GFileInfo    *dir_info,
                                  GVariant     *xattrs)
{
  GVariant *ret_metadata = NULL;

  ret_metadata = g_variant_new ("(uuuu@a(ayay))",
                                OSTREE_DIR_META_VERSION,
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::uid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::gid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::mode")),
                                xattrs ? xattrs : g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));
  g_variant_ref_sink (ret_metadata);

  return ret_metadata;
}

gboolean
ostree_set_xattrs (GFile  *f, 
                   GVariant *xattrs, 
                   GCancellable *cancellable, 
                   GError **error)
{
  const char *path;
  gboolean ret = FALSE;
  int i, n;

  path = ot_gfile_get_path_cached (f);

  n = g_variant_n_children (xattrs);
  for (i = 0; i < n; i++)
    {
      const guint8* name;
      GVariant *value;
      const guint8* value_data;
      gsize value_len;
      gboolean loop_err;

      g_variant_get_child (xattrs, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);
      
      loop_err = lsetxattr (path, (char*)name, (char*)value_data, value_len, XATTR_REPLACE) < 0;
      ot_clear_gvariant (&value);
      if (loop_err)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_map_metadata_file (GFile                       *file,
                          OstreeObjectType             expected_type,
                          GVariant                   **out_variant,
                          GError                     **error)
{
  gboolean ret = FALSE;
  GVariant *ret_variant = NULL;
  GVariant *container = NULL;
  guint32 actual_type;

  if (!ot_util_variant_map (file, OSTREE_SERIALIZED_VARIANT_FORMAT,
                            &container, error))
    goto out;

  g_variant_get (container, "(uv)",
                 &actual_type, &ret_variant);
  ot_util_variant_take_ref (ret_variant);
  actual_type = GUINT32_FROM_BE (actual_type);
  if (actual_type != expected_type)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object '%s'; found type %u, expected %u",
                   ot_gfile_get_path_cached (file), 
                   actual_type, (guint32)expected_type);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_variant, &ret_variant);
 out:
  ot_clear_gvariant (&ret_variant);
  ot_clear_gvariant (&container);
  return ret;
}

const char *
ostree_object_type_to_string (OstreeObjectType objtype)
{
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_RAW_FILE:
      return "file";
    case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT:
      return "archive-content";
    case OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META:
      return "archive-meta";
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      return "dirtree";
    case OSTREE_OBJECT_TYPE_DIR_META:
      return "dirmeta";
    case OSTREE_OBJECT_TYPE_COMMIT:
      return "commit";
    default:
      g_assert_not_reached ();
      return NULL;
    }
}

OstreeObjectType
ostree_object_type_from_string (const char *str)
{
  if (!strcmp (str, "file"))
    return OSTREE_OBJECT_TYPE_RAW_FILE;
  else if (!strcmp (str, "archive-content"))
    return OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT;
  else if (!strcmp (str, "archive-meta"))
    return OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META;
  else if (!strcmp (str, "dirtree"))
    return OSTREE_OBJECT_TYPE_DIR_TREE;
  else if (!strcmp (str, "dirmeta"))
    return OSTREE_OBJECT_TYPE_DIR_META;
  else if (!strcmp (str, "commit"))
    return OSTREE_OBJECT_TYPE_COMMIT;
  g_assert_not_reached ();
  return 0;
}

char *
ostree_object_to_string (const char *checksum,
                         OstreeObjectType objtype)
{
  return g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
}

void
ostree_object_from_string (const char *str,
                           gchar     **out_checksum,
                           OstreeObjectType *out_objtype)
{
  const char *dot;

  dot = strrchr (str, '.');
  g_assert (dot != NULL);
  *out_checksum = g_strndup (str, dot - str);
  *out_objtype = ostree_object_type_from_string (dot + 1);
}

guint
ostree_hash_object_name (gconstpointer a)
{
  GVariant *variant = (gpointer)a;
  const char *checksum;
  OstreeObjectType objtype;
  gint objtype_int;
  
  ostree_object_name_deserialize (variant, &checksum, &objtype);
  objtype_int = (gint) objtype;
  return g_str_hash (checksum) + g_int_hash (&objtype_int);
}

GVariant *
ostree_object_name_serialize (const char *checksum,
                              OstreeObjectType objtype)
{
  return g_variant_new ("(su)", checksum, (guint32)objtype);
}

void
ostree_object_name_deserialize (GVariant         *variant,
                                const char      **out_checksum,
                                OstreeObjectType *out_objtype)
{
  guint32 objtype_u32;
  g_variant_get (variant, "(&su)", out_checksum, &objtype_u32);
  *out_objtype = (OstreeObjectType)objtype_u32;
}

char *
ostree_get_relative_object_path (const char *checksum,
                                 OstreeObjectType type)
{
  GString *path;

  g_assert (strlen (checksum) == 64);

  path = g_string_new ("objects/");

  g_string_append_len (path, checksum, 2);
  g_string_append_c (path, '/');
  g_string_append (path, checksum + 2);
  g_string_append_c (path, '.');
  g_string_append (path, ostree_object_type_to_string (type));

  return g_string_free (path, FALSE);
}

GVariant *
ostree_create_archive_file_metadata (GFileInfo         *finfo,
                                     GVariant          *xattrs)
{
  guint32 uid, gid, mode, rdev;
  GVariantBuilder pack_builder;
  GVariant *ret = NULL;

  uid = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_UID);
  gid = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_GID);
  mode = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_MODE);
  rdev = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_RDEV);

  g_variant_builder_init (&pack_builder, OSTREE_ARCHIVED_FILE_VARIANT_FORMAT);
  g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (0));   /* Version */ 
  g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (uid));
  g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (gid));
  g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (mode));
  g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (rdev));
  if (S_ISLNK (mode))
    g_variant_builder_add (&pack_builder, "s", g_file_info_get_symlink_target (finfo));
  else
    g_variant_builder_add (&pack_builder, "s", "");

  g_variant_builder_add (&pack_builder, "@a(ayay)", xattrs ? xattrs : g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret = g_variant_builder_end (&pack_builder);
  g_variant_ref_sink (ret);
  
  return ret;
}

gboolean
ostree_parse_archived_file_meta (GVariant         *metadata,
                                 GFileInfo       **out_file_info,
                                 GVariant        **out_xattrs,
                                 GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *ret_file_info = NULL;
  GVariant *ret_xattrs = NULL;
  guint32 version, uid, gid, mode, rdev;
  const char *symlink_target;

  g_variant_get (metadata, "(uuuuu&s@a(ayay))",
                 &version, &uid, &gid, &mode, &rdev,
                 &symlink_target, &ret_xattrs);
  version = GUINT32_FROM_BE (version);

  if (version != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid version %d in archived file metadata", version);
      goto out;
    }

  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  rdev = GUINT32_FROM_BE (rdev);

  ret_file_info = g_file_info_new ();
  g_file_info_set_attribute_uint32 (ret_file_info, "standard::type", ot_gfile_type_for_mode (mode));
  g_file_info_set_attribute_boolean (ret_file_info, "standard::is-symlink", S_ISLNK (mode));
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::mode", mode);

  if (S_ISREG (mode))
    {
      ;
    }
  else if (S_ISLNK (mode))
    {
      g_file_info_set_attribute_byte_string (ret_file_info, "standard::symlink-target", symlink_target);
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      g_file_info_set_attribute_uint32 (ret_file_info, "unix::rdev", rdev);
    }
  else if (S_ISFIFO (mode))
    {
      ;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid mode %u", mode);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file_info, &ret_file_info);
  ot_transfer_out_value(out_xattrs, &ret_xattrs);
 out:
  g_clear_object (&ret_file_info);
  ot_clear_gvariant (&ret_xattrs);
  return ret;
}

gboolean
ostree_create_file_from_input (GFile            *dest_file,
                               GFileInfo        *finfo,
                               GVariant         *xattrs,
                               GInputStream     *input,
                               OstreeObjectType  objtype,
                               GChecksum       **out_checksum,
                               GCancellable     *cancellable,
                               GError          **error)
{
  const char *dest_path;
  gboolean ret = FALSE;
  GFileOutputStream *out = NULL;
  guint32 uid, gid, mode;
  GChecksum *ret_checksum = NULL;
  gboolean is_meta;
  gboolean is_archived_content;

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  is_archived_content = objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (finfo != NULL)
    {
      mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
      /* Archived content files should always be readable by all and
       * read/write by owner.  If the base file is executable then
       * we're also executable.
       */
      if (is_archived_content)
        mode |= 0644;
    }
  else
    {
      mode = S_IFREG | 0664;
    }
  dest_path = ot_gfile_get_path_cached (dest_file);

  if (S_ISDIR (mode))
    {
      if (mkdir (ot_gfile_get_path_cached (dest_file), mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISREG (mode))
    {
      out = g_file_create (dest_file, 0, cancellable, error);
      if (!out)
        goto out;

      if (input)
        {
          if (!ot_gio_splice_and_checksum ((GOutputStream*)out, input,
                                           out_checksum ? &ret_checksum : NULL,
                                           cancellable, error))
            goto out;
        }

      if (!g_output_stream_close ((GOutputStream*)out, NULL, error))
        goto out;
    }
  else if (S_ISLNK (mode))
    {
      const char *target = g_file_info_get_attribute_byte_string (finfo, "standard::symlink-target");
      g_assert (!is_meta);
      if (out_checksum)
        ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (ret_checksum)
        g_checksum_update (ret_checksum, (guint8*)target, strlen (target));
      if (symlink (target, dest_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      guint32 dev = g_file_info_get_attribute_uint32 (finfo, "unix::rdev");
      guint32 dev_be;
      g_assert (!is_meta);
      dev_be = GUINT32_TO_BE (dev);
      if (out_checksum)
        ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (ret_checksum)
        g_checksum_update (ret_checksum, (guint8*)&dev_be, 4);
      if (mknod (dest_path, mode, dev) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISFIFO (mode))
    {
      g_assert (!is_meta);
      if (out_checksum)
        ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (mkfifo (dest_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode %u", mode);
      goto out;
    }

  if (finfo != NULL && !is_meta && !is_archived_content)
    {
      uid = g_file_info_get_attribute_uint32 (finfo, "unix::uid");
      gid = g_file_info_get_attribute_uint32 (finfo, "unix::gid");
      
      if (lchown (dest_path, uid, gid) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!S_ISLNK (mode))
    {
      if (chmod (dest_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (xattrs != NULL)
    {
      g_assert (!is_meta);
      if (!ostree_set_xattrs (dest_file, xattrs, cancellable, error))
        goto out;
    }

  if (ret_checksum && !is_meta && !is_archived_content)
    {
      g_assert (finfo != NULL);

      ostree_checksum_update_stat (ret_checksum,
                                   g_file_info_get_attribute_uint32 (finfo, "unix::uid"),
                                   g_file_info_get_attribute_uint32 (finfo, "unix::gid"),
                                   mode);
      if (xattrs)
        g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  if (!ret && !S_ISDIR(mode))
    {
      (void) unlink (dest_path);
    }
  ot_clear_checksum (&ret_checksum);
  g_clear_object (&out);
  return ret;
}

static GString *
create_tmp_string (const char *dirpath,
                   const char *prefix,
                   const char *suffix)
{
  GString *tmp_name = NULL;

  if (!prefix)
    prefix = "tmp";
  if (!suffix)
    suffix = "tmp";

  tmp_name = g_string_new (dirpath);
  g_string_append_c (tmp_name, '/');
  g_string_append (tmp_name, prefix);
  g_string_append (tmp_name, "-XXXXXXXXXXXX.");
  g_string_append (tmp_name, suffix);

  return tmp_name;
}

static char *
subst_xxxxxx (const char *string)
{
  static const char table[] = "ABCEDEFGHIJKLMNOPQRSTUVWXYZabcedefghijklmnopqrstuvwxyz0123456789";
  char *ret = g_strdup (string);
  guint8 *xxxxxx = (guint8*)strstr (ret, "XXXXXX");

  g_assert (xxxxxx != NULL);

  while (*xxxxxx == 'X')
    {
      int offset = g_random_int_range (0, sizeof (table) - 1);
      *xxxxxx = (guint8)table[offset];
      xxxxxx++;
    }

  return ret;
}

gboolean
ostree_create_temp_file_from_input (GFile            *dir,
                                    const char       *prefix,
                                    const char       *suffix,
                                    GFileInfo        *finfo,
                                    GVariant         *xattrs,
                                    GInputStream     *input,
                                    OstreeObjectType objtype,
                                    GFile           **out_file,
                                    GChecksum       **out_checksum,
                                    GCancellable     *cancellable,
                                    GError          **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GString *tmp_name = NULL;
  char *possible_name = NULL;
  GFile *possible_file = NULL;
  GError *temp_error = NULL;
  int i = 0;

  tmp_name = create_tmp_string (ot_gfile_get_path_cached (dir),
                                prefix, suffix);
  
  /* 128 attempts seems reasonable... */
  for (i = 0; i < 128; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      g_free (possible_name);
      possible_name = subst_xxxxxx (tmp_name->str);
      g_clear_object (&possible_file);
      possible_file = g_file_get_child (dir, possible_name);
      
      if (!ostree_create_file_from_input (possible_file, finfo, xattrs, input,
                                          objtype,
                                          out_checksum ? &ret_checksum : NULL,
                                          cancellable, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_clear_error (&temp_error);
              continue;
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          break;
        }
    }
  if (i >= 128)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted 128 attempts to create a temporary file");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
  ot_transfer_out_value(out_file, &possible_file);
 out:
  if (tmp_name)
    g_string_free (tmp_name, TRUE);
  g_free (possible_name);
  g_clear_object (&possible_file);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

gboolean
ostree_create_temp_regular_file (GFile            *dir,
                                 const char       *prefix,
                                 const char       *suffix,
                                 GFile           **out_file,
                                 GOutputStream   **out_stream,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  gboolean ret = FALSE;
  GFile *ret_file = NULL;
  GOutputStream *ret_stream = NULL;

  if (!ostree_create_temp_file_from_input (dir, prefix, suffix, NULL, NULL, NULL,
                                           OSTREE_OBJECT_TYPE_RAW_FILE, &ret_file,
                                           NULL, cancellable, error))
    goto out;
  
  ret_stream = (GOutputStream*)g_file_append_to (ret_file, 0, cancellable, error);
  if (ret_stream == NULL)
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_file, &ret_file);
  ot_transfer_out_value(out_stream, &ret_stream);
 out:
  g_clear_object (&ret_file);
  g_clear_object (&ret_stream);
  return ret;
}

gboolean
ostree_create_temp_hardlink (GFile            *dir,
                             GFile            *src,
                             const char       *prefix,
                             const char       *suffix,
                             GFile           **out_file,
                             GCancellable     *cancellable,
                             GError          **error)
{
  gboolean ret = FALSE;
  GString *tmp_name = NULL;
  char *possible_name = NULL;
  GFile *possible_file = NULL;
  int i = 0;

  tmp_name = create_tmp_string (ot_gfile_get_path_cached (dir),
                                prefix, suffix);
  
  /* 128 attempts seems reasonable... */
  for (i = 0; i < 128; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      g_free (possible_name);
      possible_name = subst_xxxxxx (tmp_name->str);
      g_clear_object (&possible_file);
      possible_file = g_file_get_child (dir, possible_name);

      if (link (ot_gfile_get_path_cached (src), ot_gfile_get_path_cached (possible_file)) < 0)
        {
          if (errno == EEXIST)
            continue;
          else
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
      else
        {
          break;
        }
    }
  if (i >= 128)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted 128 attempts to create a temporary file");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file, &possible_file);
 out:
  if (tmp_name)
    g_string_free (tmp_name, TRUE);
  g_free (possible_name);
  g_clear_object (&possible_file);
  return ret;
}
