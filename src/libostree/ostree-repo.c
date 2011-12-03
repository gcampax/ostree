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

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-file-enumerator.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), OSTREE_TYPE_REPO, OstreeRepoPrivate))

typedef struct _OstreeRepoPrivate OstreeRepoPrivate;

struct _OstreeRepoPrivate {
  char *path;
  GFile *repo_file;
  GFile *tmp_dir;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  char *objects_path;
  char *config_path;

  gboolean inited;

  GKeyFile *config;
  gboolean archive;
};

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_free (priv->path);
  g_clear_object (&priv->repo_file);
  g_clear_object (&priv->tmp_dir);
  g_clear_object (&priv->local_heads_dir);
  g_clear_object (&priv->remote_heads_dir);
  g_free (priv->objects_path);
  g_free (priv->config_path);
  if (priv->config)
    g_key_file_free (priv->config);

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject *
ostree_repo_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  GObject *object;
  GObjectClass *parent_class;
  OstreeRepoPrivate *priv;

  parent_class = G_OBJECT_CLASS (ostree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);

  priv = GET_PRIVATE (object);

  g_assert (priv->path != NULL);
  
  priv->repo_file = ot_gfile_new_for_path (priv->path);
  priv->tmp_dir = g_file_resolve_relative_path (priv->repo_file, "tmp");
  priv->local_heads_dir = g_file_resolve_relative_path (priv->repo_file, "refs/heads");
  priv->remote_heads_dir = g_file_resolve_relative_path (priv->repo_file, "refs/remotes");
  
  priv->objects_path = g_build_filename (priv->path, "objects", NULL);
  priv->config_path = g_build_filename (priv->path, "config", NULL);

  return object;
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (OstreeRepoPrivate));

  object_class->constructor = ostree_repo_constructor;
  object_class->get_property = ostree_repo_get_property;
  object_class->set_property = ostree_repo_set_property;
  object_class->finalize = ostree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_repo_init (OstreeRepo *self)
{
}

OstreeRepo*
ostree_repo_new (const char *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  gboolean ret = FALSE;
  char *rev = NULL;

  if (!ot_gfile_load_contents_utf8 (f, &rev, NULL, NULL, &temp_error))
    goto out;

  if (rev == NULL)
    {
      if (g_error_matches (temp_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      g_strchomp (rev);
    }

  if (g_str_has_prefix (rev, "ref: "))
    {
      GFile *ref;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (priv->local_heads_dir, rev + 5);
      subret = parse_rev_file (self, ref, &ref_sha256, error);
      g_clear_object (&ref);
        
      if (!subret)
        {
          g_free (ref_sha256);
          goto out;
        }
      
      g_free (rev);
      rev = ref_sha256;
    }
  else 
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;
    }

  ot_transfer_out_value(sha256, rev);
  ret = TRUE;
 out:
  g_free (rev);
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         gboolean        allow_noent,
                         char          **sha256,
                         GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *tmp = NULL;
  char *tmp2 = NULL;
  char *ret_rev = NULL;
  GFile *child = NULL;
  GFile *origindir = NULL;
  const char *child_path = NULL;
  GError *temp_error = NULL;
  GVariant *commit = NULL;

  g_return_val_if_fail (rev != NULL, FALSE);

  if (strlen (rev) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty rev");
      goto out;
    }
  else if (strstr (rev, "..") != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev %s", rev);
      goto out;
    }
  else if (strlen (rev) == 64)
    {
      ret_rev = g_strdup (rev);
    }
  else if (g_str_has_suffix (rev, "^"))
    {
      tmp = g_strdup (rev);
      tmp[strlen(tmp) - 1] = '\0';

      if (!ostree_repo_resolve_rev (self, tmp, allow_noent, &tmp2, error))
        goto out;

      if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_COMMIT_VARIANT, tmp2, &commit, error))
        goto out;
      
      g_variant_get_child (commit, 2, "s", &ret_rev);
      if (strlen (ret_rev) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s has no parent", tmp2);
          goto out;

        }
    }
  else
    {
      const char *slash = strchr (rev, '/');
      if (slash != NULL && (slash == rev || !*(slash+1)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid rev %s", rev);
          goto out;
        }
      else if (slash == NULL)
        {
          child = g_file_get_child (priv->local_heads_dir, rev);
          child_path = ot_gfile_get_path_cached (child);
        }
      else
        {
          const char *rest = slash + 1;

          if (strchr (rest, '/'))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid rev %s", rev);
              goto out;
            }
          
          child = g_file_get_child (priv->remote_heads_dir, rev);
          child_path = ot_gfile_get_path_cached (child);

        }
      if (!ot_gfile_load_contents_utf8 (child, &ret_rev, NULL, NULL, &temp_error))
        {
          if (allow_noent && g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_free (ret_rev);
              ret_rev = NULL;
            }
          else
            {
              g_propagate_error (error, temp_error);
              g_prefix_error (error, "Couldn't open ref '%s': ", child_path);
              goto out;
            }
        }
      else
        {
          g_strchomp (ret_rev);
         
          if (!ostree_validate_checksum_string (ret_rev, error))
            goto out;
        }
    }

  ot_transfer_out_value(sha256, ret_rev);
  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  g_free (tmp);
  g_free (tmp2);
  g_clear_object (&child);
  g_clear_object (&origindir);
  g_free (ret_rev);
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GError **error)
{
  gboolean ret = FALSE;
  GFile *child = NULL;
  GOutputStream *out = NULL;
  gsize bytes_written;

  child = g_file_get_child (parentdir, name);

  if ((out = (GOutputStream*)g_file_replace (child, NULL, FALSE, 0, NULL, error)) == NULL)
    goto out;
  if (!g_output_stream_write_all (out, sha256, strlen (sha256), &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_write_all (out, "\n", 1, &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_close (out, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&child);
  g_clear_object (&out);
  return ret;
}

/**
 * ostree_repo_get_config:
 * @self:
 *
 * Returns: (transfer none): The repository configuration; do not modify
 */
GKeyFile *
ostree_repo_get_config (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, NULL);

  return priv->config;
}

/**
 * ostree_repo_copy_config:
 * @self:
 *
 * Returns: (transfer full): A newly-allocated copy of the repository config
 */
GKeyFile *
ostree_repo_copy_config (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (priv->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (priv->config, &len, NULL);
  if (!g_key_file_load_from_data (copy, data, len, 0, NULL))
    g_assert_not_reached ();
  g_free (data);
  return copy;
}

/**
 * ostree_repo_write_config:
 * @self:
 * @new_config: Overwrite the config file with this data.  Do not change later!
 * @error: a #GError
 *
 * Save @new_config in place of this repository's config file.  Note
 * that @new_config should not be modified after - this function
 * simply adds a reference.
 */
gboolean
ostree_repo_write_config (OstreeRepo *self,
                          GKeyFile   *new_config,
                          GError    **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *data = NULL;
  gsize len;
  gboolean ret = FALSE;

  g_return_val_if_fail (priv->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_set_contents (priv->config_path, data, len, error))
    goto out;
  
  g_key_file_free (priv->config);
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_data (priv->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
  g_free (data);
  return ret;
}

gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *version = NULL;;
  GError *temp_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (priv->inited)
    return TRUE;

  if (!g_file_test (priv->objects_path, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'", priv->objects_path);
      goto out;
    }
  
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_file (priv->config, priv->config_path, 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      goto out;
    }

  version = g_key_file_get_value (priv->config, "core", "repo_version", &temp_error);
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (strcmp (version, "0") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  priv->archive = g_key_file_get_boolean (priv->config, "core", "archive", &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  priv->inited = TRUE;
  
  ret = TRUE;
 out:
  g_free (version);
  return ret;
}

const char *
ostree_repo_get_path (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->path;
}

GFile *
ostree_repo_get_tmpdir (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->tmp_dir;
}

gboolean      
ostree_repo_is_archive (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, FALSE);

  return priv->archive;
}

static gboolean
stage_and_checksum (OstreeRepo       *self,
                    OstreeObjectType  objtype,
                    GInputStream     *input,
                    GFile           **out_tmpname,
                    GChecksum       **out_checksum,
                    GCancellable     *cancellable,
                    GError          **error)
{
  gboolean ret = FALSE;
  const char *prefix = NULL;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *tmp_f = NULL;
  GOutputStream *stream = NULL;
  GFile *ret_tmpname = NULL;
  GChecksum *ret_checksum = NULL;
  
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_FILE:
      prefix = "file-tmp-";
      break;
    case OSTREE_OBJECT_TYPE_META:
      prefix = "meta-tmp-";
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  if (!ostree_create_temp_file_from_input (priv->tmp_dir, prefix, NULL, NULL,
                                           NULL, input, OSTREE_OBJECT_TYPE_META,
                                           &tmp_f, &ret_checksum,
                                           cancellable, error))
    goto out;

  ret_tmpname = g_file_get_child (priv->tmp_dir, g_checksum_get_string (ret_checksum));
  if (rename (ot_gfile_get_path_cached (tmp_f), ot_gfile_get_path_cached (ret_tmpname)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
  /* Clear it here, since we know the file is now gone */ 
  g_clear_object (&tmp_f);

  ret = TRUE;
  ot_transfer_out_value(out_tmpname, ret_tmpname);
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  if (tmp_f)
    (void) unlink (ot_gfile_get_path_cached (tmp_f));
  g_clear_object (&tmp_f);
  g_clear_object (&stream);
  g_clear_object (&ret_tmpname);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
write_gvariant_to_tmp (OstreeRepo  *self,
                       OstreeSerializedVariantType type,
                       GVariant    *variant,
                       GFile        **out_tmpname,
                       GChecksum    **out_checksum,
                       GCancellable  *cancellable,
                       GError       **error)
{
  gboolean ret = FALSE;
  GVariant *serialized = NULL;
  GInputStream *mem = NULL;
  GChecksum *ret_checksum = NULL;
  GFile *ret_tmpname = NULL;

  serialized = ostree_wrap_metadata_variant (type, variant);
  mem = g_memory_input_stream_new_from_data (g_variant_get_data (serialized),
                                             g_variant_get_size (serialized),
                                             NULL);
  if (!stage_and_checksum (self, OSTREE_OBJECT_TYPE_META,
                           mem, &ret_tmpname, &ret_checksum, cancellable, error))
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_tmpname, ret_tmpname);
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  g_clear_object (&ret_tmpname);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
import_gvariant_object (OstreeRepo  *self,
                        OstreeSerializedVariantType type,
                        GVariant       *variant,
                        GChecksum    **out_checksum,
                        GCancellable *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  GFile *tmp_path = NULL;
  GChecksum *ret_checksum = NULL;
  gboolean did_exist;
  
  if (!write_gvariant_to_tmp (self, type, variant, &tmp_path, &ret_checksum, cancellable, error))
    goto out;

  if (!ostree_repo_store_object_trusted (self, tmp_path,
                                         g_checksum_get_string (ret_checksum),
                                         OSTREE_OBJECT_TYPE_META,
                                         FALSE, &did_exist, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  (void) unlink (ot_gfile_get_path_cached (tmp_path));
  g_clear_object (&tmp_path);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

gboolean
ostree_repo_load_variant_checked (OstreeRepo  *self,
                                  OstreeSerializedVariantType expected_type,
                                  const char    *sha256, 
                                  GVariant     **out_variant,
                                  GError       **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType type;
  GVariant *ret_variant = NULL;

  if (!ostree_repo_load_variant (self, sha256, &type, &ret_variant, error))
    goto out;

  if (type != expected_type)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object '%s'; found type %u, expected %u", sha256,
                   type, (guint32)expected_type);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_variant, ret_variant);
 out:
  ot_clear_gvariant (&ret_variant);
  return ret;
}

static gboolean
import_directory_meta (OstreeRepo   *self,
                       GFileInfo    *file_info,
                       GVariant     *xattrs,
                       GChecksum   **out_checksum,
                       GCancellable *cancellable,
                       GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_DIRMETA_VARIANT, 
                               dirmeta, &ret_checksum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  ot_clear_checksum (&ret_checksum);
  ot_clear_gvariant (&dirmeta);
  return ret;
}

GFile *
ostree_repo_get_object_path (OstreeRepo  *self,
                             const char    *checksum,
                             OstreeObjectType type)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *path;
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, type, priv->archive);
  path = g_build_filename (priv->path, relpath, NULL);
  g_free (relpath);
  ret = ot_gfile_new_for_path (path);
  g_free (path);
 
  return ret;
}

static gboolean
prepare_dir_for_checksum_get_object_path (OstreeRepo *self,
                                          const char   *checksum,
                                          OstreeObjectType type,
                                          GFile  **out_file,
                                          GError      **error)
{
  gboolean ret = FALSE;
  GFile *checksum_dir = NULL;
  GFile *ret_file = NULL;

  ret_file = ostree_repo_get_object_path (self, checksum, type);
  checksum_dir = g_file_get_parent (ret_file);

  if (!ot_gfile_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_file, ret_file);
 out:
  g_clear_object (&checksum_dir);
  g_clear_object (&ret_file);
  return ret;
}

static gboolean
link_object_trusted (OstreeRepo   *self,
                     GFile        *file,
                     const char   *checksum,
                     OstreeObjectType objtype,
                     gboolean      overwrite,
                     gboolean     *did_exist,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  char *dest_basename = NULL;
  char *tmp_dest_path = NULL;
  const char *dest_path = NULL;
  GFile *dest_file = NULL;

  if (!prepare_dir_for_checksum_get_object_path (self, checksum, objtype, &dest_file, error))
    goto out;

  *did_exist = g_file_query_exists (dest_file, NULL);
  if (!overwrite && *did_exist)
    {
      ;
    }
  else
    {
      dest_path = ot_gfile_get_path_cached (dest_file);
      dest_basename = g_path_get_basename (dest_path);
      tmp_dest_path = g_strconcat (dest_path, ".tmp", NULL);

      (void) unlink (tmp_dest_path);

      if (link (ot_gfile_get_path_cached (file), tmp_dest_path) < 0
          || rename (tmp_dest_path, dest_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          ot_gfile_get_path_cached (file));
          goto out;
        }
      g_free (tmp_dest_path);
      tmp_dest_path = NULL;
    }

  ret = TRUE;
 out:
  g_free (dest_basename);
  if (tmp_dest_path)
    (void) unlink (tmp_dest_path);
  g_free (tmp_dest_path);
  g_clear_object (&dest_file);
  return ret;
}

static gboolean
archive_file_trusted (OstreeRepo   *self,
                      GFile        *file,
                      const char   *checksum,
                      gboolean      overwrite,
                      gboolean     *did_exist,
                      GCancellable *cancellable,
                      GError      **error)
{
  GFileOutputStream *out = NULL;
  gboolean ret = FALSE;
  GFile *dest_file = NULL;
  GFileInfo *finfo = NULL;
  GInputStream *input = NULL;
  GError *temp_error = NULL;

  if (!prepare_dir_for_checksum_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE, &dest_file, error))
    goto out;

  if (overwrite)
    {
      out = g_file_replace (dest_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, cancellable, error);
      if (!out)
        goto out;
    }
  else
    {
      out = g_file_create (dest_file, 0, cancellable, &temp_error);
      if (!out)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_clear_error (&temp_error);
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
    }

  if (out)
    {
      if (!ostree_pack_file ((GOutputStream*)out, file, cancellable, error))
        goto out;
      
      if (!g_output_stream_close ((GOutputStream*)out, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dest_file);
  g_clear_object (&out);
  g_clear_object (&finfo);
  g_clear_object (&input);
  return ret;
}
  
gboolean      
ostree_repo_store_object_trusted (OstreeRepo   *self,
                                  GFile        *file,
                                  const char   *checksum,
                                  OstreeObjectType objtype,
                                  gboolean      overwrite,
                                  gboolean     *did_exist,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  if (priv->archive && objtype == OSTREE_OBJECT_TYPE_FILE)
    return archive_file_trusted (self, file, checksum, overwrite, did_exist, cancellable, error);
  else
    return link_object_trusted (self, file, checksum, objtype, overwrite, did_exist, cancellable, error);
}

static gboolean
ostree_repo_store_file (OstreeRepo         *self,
                        GFile              *file,
                        GFileInfo          *file_info,
                        GChecksum         **out_checksum,
                        gboolean           *did_exist,
                        GCancellable       *cancellable,
                        GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GVariant *xattrs = NULL;
  GInputStream *input = NULL;
  GOutputStream *temp_out = NULL;
  GChecksum *ret_checksum = NULL;
  GFile *temp_file = NULL;

  if (priv->archive && g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
       /* Avoid reading the input data twice for regular files in the
          archive case */
      input = (GInputStream*)g_file_read (file, cancellable, error);
      if (input == NULL)
        goto out;

      xattrs = ostree_get_xattrs_for_file (file, error);
      if (!xattrs)
        goto out;
      
      if (!ostree_create_temp_regular_file (priv->tmp_dir,
                                            "archive-tmp-", NULL,
                                            &temp_file, &temp_out,
                                            cancellable, error))
        goto out;
      
      if (!ostree_pack_file_for_input (temp_out, file_info, input, xattrs,
                                       &ret_checksum, cancellable, error))
        goto out;

      if (!g_output_stream_close (temp_out, cancellable, error))
        goto out;

      if (!link_object_trusted (self, temp_file,
                                g_checksum_get_string (ret_checksum),
                                OSTREE_OBJECT_TYPE_FILE, FALSE, did_exist,
                                cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_checksum_file (file, OSTREE_OBJECT_TYPE_FILE, &ret_checksum, cancellable, error))
        goto out;
      
      if (!ostree_repo_store_object_trusted (self, file, g_checksum_get_string (ret_checksum),
                                             OSTREE_OBJECT_TYPE_FILE, FALSE, did_exist,
                                             cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  g_clear_object (&temp_file);
  g_clear_object (&temp_out);
  g_clear_object (&input);
  ot_clear_checksum (&ret_checksum);
  ot_clear_gvariant (&xattrs);
  return ret;
}

gboolean
ostree_repo_store_packfile (OstreeRepo       *self,
                            const char       *expected_checksum,
                            const char       *path,
                            OstreeObjectType  objtype,
                            gboolean         *did_exist,
                            GError          **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GChecksum *checksum = NULL;
  GFile *src = NULL;
  GFile *tempfile = NULL;

  src = ot_gfile_new_for_path (path);
  tempfile = g_file_get_child (priv->tmp_dir, expected_checksum);
  
  if (!ostree_unpack_object (src, objtype, tempfile, &checksum, error))
    goto out;

  if (strcmp (g_checksum_get_string (checksum), expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted object %s (actual checksum is %s)",
                   expected_checksum, g_checksum_get_string (checksum));
      goto out;
    }

  if (!ostree_repo_store_object_trusted (self, tempfile,
                                         expected_checksum,
                                         objtype,
                                         FALSE, did_exist, NULL, error))
    goto out;

  ret = TRUE;
 out:
  if (tempfile)
    (void) unlink (ot_gfile_get_path_cached (tempfile));
  g_clear_object (&tempfile);
  g_clear_object (&src);
  ot_clear_checksum (&checksum);
  return ret;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

gboolean      
ostree_repo_write_ref (OstreeRepo  *self,
                       const char  *remote,
                       const char  *name,
                       const char  *rev,
                       GError     **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *dir = NULL;

  if (remote == NULL)
    dir = g_object_ref (priv->local_heads_dir);
  else
    {
      dir = g_file_get_child (priv->remote_heads_dir, remote);

      if (!ot_gfile_ensure_directory (dir, FALSE, error))
        goto out;
    }

  if (!write_checksum_file (dir, name, rev, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&dir);
  return ret;
}

static gboolean
import_commit (OstreeRepo *self,
               const char   *branch,
               const char   *parent,
               const char   *subject,
               const char   *body,
               GVariant     *metadata,
               GChecksum    *root_contents_checksum,
               GChecksum    *root_metadata_checksum,
               GChecksum   **out_commit,
               GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_commit = NULL;
  GVariant *commit = NULL;
  GDateTime *now = NULL;

  g_assert (branch != NULL);
  g_assert (subject != NULL);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(u@a{sv}ssstss)",
                          GUINT32_TO_BE (OSTREE_COMMIT_VERSION),
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? parent : "",
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          g_checksum_get_string (root_contents_checksum),
                          g_checksum_get_string (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_COMMIT_VARIANT,
                               commit, &ret_commit, NULL, error))
    goto out;

  if (!ostree_repo_write_ref (self, NULL, branch, g_checksum_get_string (ret_commit), error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_commit, ret_commit);
 out:
  ot_clear_checksum (&ret_commit);
  ot_clear_gvariant (&commit);
  if (now)
    g_date_time_unref (now);
  return ret;
}

static gboolean
import_directory_recurse (OstreeRepo           *self,
                          GFile                *base,
                          GFile                *dir,
                          GChecksum           **out_contents_checksum,
                          GChecksum           **out_metadata_checksum,
                          GCancellable         *cancellable,
                          GError              **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GChecksum *ret_metadata_checksum = NULL;
  GChecksum *ret_contents_checksum = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFileInfo *child_info = NULL;
  GFile *child = NULL;
  GHashTable *file_checksums = NULL;
  GHashTable *dir_metadata_checksums = NULL;
  GHashTable *dir_contents_checksums = NULL;
  GChecksum *child_file_checksum = NULL;
  gboolean did_exist;
  gboolean builders_initialized = FALSE;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GHashTableIter hash_iter;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  GVariant *dir_xattrs = NULL;
  GVariant *serialized_tree = NULL;
  gpointer key, value;

  child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  cancellable, error);
  if (!child_info)
    goto out;

  dir_xattrs = ostree_get_xattrs_for_file (dir, error);
  if (!dir_xattrs)
    goto out;

  if (!import_directory_meta (self, child_info, dir_xattrs, &ret_metadata_checksum, cancellable, error))
    goto out;
  
  g_clear_object (&child_info);

  dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;
  
  file_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          (GDestroyNotify)g_free, (GDestroyNotify)g_free);
  dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);
  dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          GChecksum *child_dir_metadata_checksum = NULL;
          GChecksum *child_dir_contents_checksum = NULL;

          if (!import_directory_recurse (self, base, child, &child_dir_contents_checksum,
                                         &child_dir_metadata_checksum, cancellable, error))
            goto out;

          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_dir_contents_checksum)));
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_dir_metadata_checksum)));
          ot_clear_checksum (&child_dir_contents_checksum);
          ot_clear_checksum (&child_dir_metadata_checksum);
        }
      else
        {
          ot_clear_checksum (&child_file_checksum);

          if (!ostree_repo_store_file (self, child, child_info, &child_file_checksum, &did_exist, cancellable, error))
            goto out;

          g_hash_table_replace (file_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_file_checksum)));
        }

      g_clear_object (&child_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(ss)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sss)"));
  builders_initialized = TRUE;

  g_hash_table_iter_init (&hash_iter, file_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(ss)", name, value);
    }
  
  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, dir_metadata_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;

      g_variant_builder_add (&dirs_builder, "(sss)",
                             name,
                             g_hash_table_lookup (dir_contents_checksums, name),
                             g_hash_table_lookup (dir_metadata_checksums, name));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(u@a{sv}@a(ss)@a(sss))",
                                   GUINT32_TO_BE (0),
                                   create_empty_gvariant_dict (),
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  builders_initialized = FALSE;
  g_variant_ref_sink (serialized_tree);

  if (!import_gvariant_object (self, OSTREE_SERIALIZED_TREE_VARIANT,
                               serialized_tree, &ret_contents_checksum,
                               cancellable, error))
    goto out;

  ot_transfer_out_value(out_metadata_checksum, ret_metadata_checksum);
  ot_transfer_out_value(out_contents_checksum, ret_contents_checksum);
  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&child);
  g_clear_object (&child_info);
  if (file_checksums)
    g_hash_table_destroy (file_checksums);
  if (dir_metadata_checksums)
    g_hash_table_destroy (dir_metadata_checksums);
  if (dir_contents_checksums)
    g_hash_table_destroy (dir_contents_checksums);
  ot_clear_checksum (&ret_metadata_checksum);
  ot_clear_checksum (&ret_contents_checksum);
  ot_clear_checksum (&child_file_checksum);
  g_slist_free (sorted_filenames);
  if (builders_initialized)
    {
      g_variant_builder_clear (&files_builder);
      g_variant_builder_clear (&dirs_builder);
    }
  ot_clear_gvariant (&serialized_tree);
  return ret;
}

gboolean      
ostree_repo_commit_directory (OstreeRepo *self,
                              const char   *branch,
                              const char   *parent,
                              const char   *subject,
                              const char   *body,
                              GVariant     *metadata,
                              GFile        *dir,
                              GChecksum   **out_commit,
                              GCancellable *cancellable,
                              GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GChecksum *ret_commit_checksum = NULL;
  GChecksum *root_metadata_checksum = NULL;
  GChecksum *root_contents_checksum = NULL;
  char *current_head = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (metadata == NULL || g_variant_is_of_type (metadata, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (parent == NULL)
    parent = branch;

  if (!ostree_repo_resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  if (!import_directory_recurse (self, dir, dir, &root_contents_checksum, &root_metadata_checksum, cancellable, error))
    goto out;

  if (!import_commit (self, branch, current_head, subject, body, metadata,
                      root_contents_checksum, root_metadata_checksum, &ret_commit_checksum, error))
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_commit, ret_commit_checksum);
 out:
  ot_clear_checksum (&ret_commit_checksum);
  g_free (current_head);
  ot_clear_checksum (&root_metadata_checksum);
  ot_clear_checksum (&root_contents_checksum);
  return ret;
}

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static GFileInfo *
file_info_from_archive_entry (struct archive_entry  *entry)
{
  GFileInfo *info = g_file_info_new ();
  guint32 mode;
  guint32 file_type;

  mode = archive_entry_mode (entry);
  file_type = ot_gfile_type_for_mode (mode);
  g_file_info_set_attribute_boolean (info, "standard::is-symlink", S_ISLNK (mode));
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);
  g_file_info_set_attribute_uint32 (info, "unix::uid", archive_entry_uid (entry));
  g_file_info_set_attribute_uint32 (info, "unix::gid", archive_entry_gid (entry));
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", (guint64) archive_entry_size (entry));
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", archive_entry_symlink (entry));
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      g_file_info_set_attribute_uint32 (info, "unix::rdev", archive_entry_rdev (entry));
    }

  return info;
}

static gboolean
import_libarchive_entry_file_to_packed (OstreeRepo           *self,
                                        struct archive       *a,
                                        struct archive_entry *entry,
                                        GFileInfo            *file_info,
                                        GChecksum           **out_checksum,
                                        GCancellable         *cancellable,
                                        GError              **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *temp_file = NULL;
  GInputStream *archive_stream = NULL;
  GOutputStream *temp_out = NULL;
  GChecksum *ret_checksum = NULL;
  gboolean did_exist;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!ostree_create_temp_regular_file (priv->tmp_dir,
                                        "archive-tmp-", NULL,
                                        &temp_file, &temp_out,
                                        cancellable, error))
        goto out;
  
  if (S_ISREG (g_file_info_get_attribute_uint32 (file_info, "unix::mode")))
    archive_stream = ostree_libarchive_input_stream_new (a);
      
  if (!ostree_pack_file_for_input (temp_out, file_info, archive_stream, 
                                   NULL, &ret_checksum, cancellable, error))
    goto out;

  if (!g_output_stream_close (temp_out, cancellable, error))
    goto out;
  
  if (!link_object_trusted (self, temp_file, g_checksum_get_string (ret_checksum),
                            OSTREE_OBJECT_TYPE_FILE,
                            FALSE, &did_exist, cancellable, error))
    goto out;

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  g_clear_object (&temp_file);
  g_clear_object (&temp_out);
  g_clear_object (&archive_stream);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
import_libarchive_entry_file (OstreeRepo           *self,
                              struct archive       *a,
                              struct archive_entry *entry,
                              GFileInfo            *file_info,
                              GChecksum           **out_checksum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *temp_file = NULL;
  GInputStream *archive_stream = NULL;
  GChecksum *ret_checksum = NULL;
  gboolean did_exist;
  guint32 mode;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  if (S_ISREG (mode))
    archive_stream = ostree_libarchive_input_stream_new (a);
  
  if (!ostree_create_temp_file_from_input (priv->tmp_dir, "file-", NULL,
                                           file_info, NULL, archive_stream, 
                                           OSTREE_OBJECT_TYPE_FILE, &temp_file,
                                           &ret_checksum, cancellable, error))
    goto out;
  
  if (!link_object_trusted (self, temp_file, g_checksum_get_string (ret_checksum),
                            OSTREE_OBJECT_TYPE_FILE, FALSE, &did_exist,
                            cancellable, error))
    goto out;

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  g_clear_object (&temp_file);
  g_clear_object (&archive_stream);
  ot_clear_checksum (&ret_checksum);
  return ret;
}
  
static gboolean
import_libarchive (OstreeRepo           *self,
                   GFile                *archive_f,
                   GChecksum           **out_contents_checksum,
                   GChecksum           **out_metadata_checksum,
                   GCancellable         *cancellable,
                   GError              **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GChecksum *ret_contents_checksum = NULL;
  GChecksum *ret_metadata_checksum = NULL;
  struct archive *a;
  struct archive_entry *entry;
  GFileInfo *file_info = NULL;
  GHashTable *file_checksums = NULL;
  GHashTable *dir_metadata_checksums = NULL;
  GHashTable *dir_contents_checksums = NULL;
  GHashTable *dir_contents = NULL;
  GChecksum *tmp_checksum = NULL;
  char *parent_path = NULL;

  a = archive_read_new ();
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, ot_gfile_get_path_cached (archive_f), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  file_checksums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  dir_contents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

  g_hash_table_insert (dir_contents, g_strdup ("/"), g_ptr_array_new_with_free_func (g_free));

  while (archive_read_next_header (a, &entry) == ARCHIVE_OK)
    {
      const char *pathname;
      guint32 mode;
      GPtrArray *parent_contents;

      g_clear_object (&file_info);
      file_info = file_info_from_archive_entry (entry);
      pathname = archive_entry_pathname (entry); 
      g_free (parent_path);
      parent_path = g_path_get_dirname (pathname);
      if (strcmp (parent_path, ".") == 0)
        *parent_path = '/';

      parent_contents = g_hash_table_lookup (dir_contents, parent_path);
      if (!parent_contents)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No such file or directory: %s", parent_path);
          goto out;
        }

      mode = archive_entry_mode (entry);

      ot_clear_checksum (&tmp_checksum);

      if (S_ISDIR (mode))
        {
          GPtrArray *contents = g_hash_table_lookup (dir_contents, pathname);
          
          if (contents)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Directory already exists: %s", pathname);
              goto out;
            }

          g_hash_table_insert (dir_contents, g_path_get_basename (pathname),
                               g_ptr_array_new_with_free_func (g_free));

          if (!import_directory_meta (self, file_info, NULL, &tmp_checksum, cancellable, error))
            goto out;

          g_hash_table_insert (dir_metadata_checksums,
                               g_strdup (pathname),
                               g_strdup (g_checksum_get_string (tmp_checksum)));
        }
      else 
        {
          if (priv->archive)
            {
              if (!import_libarchive_entry_file_to_packed (self, a, entry, file_info, &tmp_checksum, cancellable, error))
                goto out;
            }
          else
            {
              if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_checksum, cancellable, error))
                goto out;
            }

          g_hash_table_insert (file_checksums,
                               g_strdup (pathname),
                               g_strdup (g_checksum_get_string (tmp_checksum)));
          g_ptr_array_add (parent_contents, g_path_get_basename (pathname));
        }
    }
  if (archive_read_finish(a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
  *out_contents_checksum = ret_contents_checksum;
  ret_contents_checksum = NULL;
  *out_metadata_checksum = ret_metadata_checksum;
  ret_metadata_checksum = NULL;
 out:
  g_hash_table_destroy (file_checksums);
  g_hash_table_destroy (dir_metadata_checksums);
  g_hash_table_destroy (dir_contents_checksums);
  g_hash_table_destroy (dir_contents);
  g_clear_object (&file_info);
  g_free (parent_path);
  ot_clear_checksum (&tmp_checksum);
  return ret;
}
#endif
  
gboolean      
ostree_repo_commit_tarfile (OstreeRepo *self,
                            const char   *branch,
                            const char   *parent,
                            const char   *subject,
                            const char   *body,
                            GVariant     *metadata,
                            GFile        *path,
                            GChecksum   **out_commit,
                            GCancellable *cancellable,
                            GError      **error)
{
#ifdef HAVE_LIBARCHIVE
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GChecksum *ret_commit_checksum = NULL;
  GChecksum *root_metadata_checksum = NULL;
  GChecksum *root_contents_checksum = NULL;
  char *current_head = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (metadata == NULL || g_variant_is_of_type (metadata, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (parent == NULL)
    parent = branch;

  if (!ostree_repo_resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  if (!import_libarchive (self, path, &root_contents_checksum, &root_metadata_checksum, cancellable, error))
    goto out;

  if (!import_commit (self, branch, current_head, subject, body, metadata,
                      root_contents_checksum, root_metadata_checksum, &ret_commit_checksum, error))
    goto out;
  
  ret = TRUE;
  *out_commit = ret_commit_checksum;
  ret_commit_checksum = NULL;
 out:
  ot_clear_checksum (&ret_commit_checksum);
  g_free (current_head);
  ot_clear_checksum (&root_metadata_checksum);
  ot_clear_checksum (&root_contents_checksum);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

static gboolean
iter_object_dir (OstreeRepo             *self,
                 GFile                  *dir,
                 OstreeRepoObjectIter    callback,
                 gpointer                user_data,
                 GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *file_info = NULL;
  const char *dirname = NULL;

  dirname = ot_gfile_get_basename_cached (dir);

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      char *dot;
      GFile *child;
      GString *checksum = NULL;
      OstreeObjectType objtype;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        goto loop_out;
      
      if (g_str_has_suffix (name, ".meta"))
        objtype = OSTREE_OBJECT_TYPE_META;
      else if (g_str_has_suffix (name, ".file")
               || g_str_has_suffix (name, ".packfile"))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else
        goto loop_out;
          
      dot = strrchr (name, '.');
      g_assert (dot);

      if ((dot - name) != 62)
        goto loop_out;
      
      checksum = g_string_new (dirname);
      g_string_append_len (checksum, name, 62);
      
      child = g_file_get_child (dir, name);
      callback (self, checksum->str, objtype, child, file_info, user_data);
      
    loop_out:
      if (checksum)
        g_string_free (checksum, TRUE);
      g_clear_object (&file_info);
      g_clear_object (&child);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&file_info);
  return ret;
}

gboolean
ostree_repo_iter_objects (OstreeRepo  *self,
                          OstreeRepoObjectIter callback,
                          gpointer       user_data,
                          GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *objectdir = NULL;
  GFileEnumerator *enumerator = NULL;
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);

  objectdir = ot_gfile_new_for_path (priv->objects_path);
  enumerator = g_file_enumerate_children (objectdir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          GFile *objdir = g_file_get_child (objectdir, name);
          if (!iter_object_dir (self, objdir, callback, user_data, error))
            {
              g_object_unref (objdir);
              goto out;
            }
          g_object_unref (objdir);
        }
      g_object_unref (file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&file_info);
  g_clear_object (&enumerator);
  g_clear_object (&objectdir);
  return ret;
}

gboolean
ostree_repo_load_variant (OstreeRepo *self,
                          const char   *sha256,
                          OstreeSerializedVariantType *out_type,
                          GVariant    **out_variant,
                          GError      **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType ret_type;
  GVariant *ret_variant = NULL;
  GFile *f = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  f = ostree_repo_get_object_path (self, sha256, OSTREE_OBJECT_TYPE_META);
  if (!ostree_parse_metadata_file (f, &ret_type, &ret_variant, error))
    goto out;

  ret = TRUE;
  if (out_type)
    *out_type = ret_type;
  ot_transfer_out_value(out_variant, ret_variant);
 out:
  ot_clear_gvariant (&ret_variant);
  g_clear_object (&f);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               OstreeRepoFile *dir,
               const char      *destination,
               GCancellable    *cancellable,
               GError         **error);

static gboolean
checkout_one_directory (OstreeRepo  *self,
                        const char *destination,
                        const char *dirname,
                        OstreeRepoFile *dir,
                        GFileInfo      *dir_info,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  GFile *dest_file = NULL;
  char *dest_path = NULL;
  GVariant *xattr_variant = NULL;

  dest_path = g_build_filename (destination, dirname, NULL);
  dest_file = ot_gfile_new_for_path (dest_path);

  if (!_ostree_repo_file_get_xattrs (dir, &xattr_variant, NULL, error))
    goto out;

  if (mkdir (dest_path, (mode_t)g_file_info_get_attribute_uint32 (dir_info, "unix::mode")) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      g_prefix_error (error, "Failed to create directory '%s': ", dest_path);
      goto out;
    }

  if (!ostree_set_xattrs (dest_file, xattr_variant, cancellable, error))
    goto out;
      
  if (!checkout_tree (self, dir, dest_path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&dest_file);
  g_free (dest_path);
  ot_clear_gvariant (&xattr_variant);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               OstreeRepoFile *dir,
               const char      *destination,
               GCancellable    *cancellable,
               GError         **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileInfo *file_info = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFile *destination_f = NULL;
  GFile *child = NULL;
  GFile *object_path = NULL;
  GFile *dest_path = NULL;

  destination_f = ot_gfile_new_for_path (destination);

  dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      child = g_file_get_child ((GFile*)dir, name);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!checkout_one_directory (self, destination, name, (OstreeRepoFile*)child, file_info, cancellable, error))
            goto out;
        }
      else
        {
          const char *checksum = _ostree_repo_file_get_checksum ((OstreeRepoFile*)child);

          dest_path = g_file_get_child (destination_f, name);
          object_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);

          if (priv->archive)
            {
              if (!ostree_unpack_object (object_path, OSTREE_OBJECT_TYPE_FILE, dest_path, NULL, error))
                goto out;
            }
          else
            {
              if (link (ot_gfile_get_path_cached (object_path), ot_gfile_get_path_cached (dest_path)) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }
            }
        }

      g_clear_object (&object_path);
      g_clear_object (&dest_path);
      g_clear_object (&file_info);
      g_clear_object (&child);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&file_info);
  g_clear_object (&child);
  g_clear_object (&object_path);
  g_clear_object (&dest_path);
  g_clear_object (&destination_f);
  g_free (dest_path);
  return ret;
}

gboolean
ostree_repo_checkout (OstreeRepo *self,
                      const char   *rev,
                      const char   *destination,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  char *resolved = NULL;
  OstreeRepoFile *root = NULL;
  GFileInfo *root_info = NULL;

  if (g_file_test (destination, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Destination path '%s' already exists",
                   destination);
      goto out;
    }

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved, error))
    goto out;

  root = (OstreeRepoFile*)_ostree_repo_file_new_root (self, resolved);
  if (!_ostree_repo_file_ensure_resolved (root, error))
    goto out;

  root_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, error);
  if (!root_info)
    goto out;

  if (!checkout_one_directory (self, destination, NULL, root, root_info, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_free (resolved);
  g_clear_object (&root);
  g_clear_object (&root_info);
  return ret;
}

static gboolean
get_file_checksum (GFile  *f,
                   GFileInfo *f_info,
                   char  **out_checksum,
                   GCancellable *cancellable,
                   GError   **error)
{
  gboolean ret = FALSE;
  GChecksum *tmp_checksum = NULL;
  char *ret_checksum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (_ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_FILE,
                                 &tmp_checksum, cancellable, error))
        goto out;
      ret_checksum = g_strdup (g_checksum_get_string (tmp_checksum));
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, ret_checksum);
 out:
  ot_clear_checksum (&tmp_checksum);
  return ret;
}

OstreeRepoDiffItem *
ostree_repo_diff_item_ref (OstreeRepoDiffItem *diffitem)
{
  g_atomic_int_inc (&diffitem->refcount);
  return diffitem;
}

void
ostree_repo_diff_item_unref (OstreeRepoDiffItem *diffitem)
{
  if (!g_atomic_int_dec_and_test (&diffitem->refcount))
    return;

  g_clear_object (&diffitem->src);
  g_clear_object (&diffitem->target);
  g_clear_object (&diffitem->src_info);
  g_clear_object (&diffitem->target_info);
  g_free (diffitem->src_checksum);
  g_free (diffitem->target_checksum);
  g_free (diffitem);
}

static OstreeRepoDiffItem *
diff_item_new (GFile          *a,
               GFileInfo      *a_info,
               GFile          *b,
               GFileInfo      *b_info,
               char           *checksum_a,
               char           *checksum_b)
{
  OstreeRepoDiffItem *ret = g_new0 (OstreeRepoDiffItem, 1);
  ret->refcount = 1;
  ret->src = a ? g_object_ref (a) : NULL;
  ret->src_info = a_info ? g_object_ref (a_info) : NULL;
  ret->target = b ? g_object_ref (b) : NULL;
  ret->target_info = b_info ? g_object_ref (b_info) : b_info;
  ret->src_checksum = g_strdup (checksum_a);
  ret->target_checksum = g_strdup (checksum_b);
  return ret;
}
               

static gboolean
diff_files (GFile          *a,
            GFileInfo      *a_info,
            GFile          *b,
            GFileInfo      *b_info,
            OstreeRepoDiffItem **out_item,
            GCancellable   *cancellable,
            GError        **error)
{
  gboolean ret = FALSE;
  char *checksum_a = NULL;
  char *checksum_b = NULL;
  OstreeRepoDiffItem *ret_item = NULL;

  if (!get_file_checksum (a, a_info, &checksum_a, cancellable, error))
    goto out;
  if (!get_file_checksum (b, b_info, &checksum_b, cancellable, error))
    goto out;

  if (strcmp (checksum_a, checksum_b) != 0)
    {
      ret_item = diff_item_new (a, a_info, b, b_info,
                                checksum_a, checksum_b);
    }

  ret = TRUE;
  ot_transfer_out_value(out_item, ret_item);
 out:
  if (ret_item)
    ostree_repo_diff_item_unref (ret_item);
  g_free (checksum_a);
  g_free (checksum_b);
  return ret;
}

static gboolean
diff_add_dir_recurse (GFile          *d,
                      GPtrArray      *added,
                      GCancellable   *cancellable,
                      GError        **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *dir_enum = NULL;
  GError *temp_error = NULL;
  GFile *child = NULL;
  GFileInfo *child_info = NULL;

  dir_enum = g_file_enumerate_children (d, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (d, name);

      g_ptr_array_add (added, g_object_ref (child));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!diff_add_dir_recurse (child, added, cancellable, error))
            goto out;
        }
      
      g_clear_object (&child_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&child_info);
  g_clear_object (&child);
  g_clear_object (&dir_enum);
  return ret;
}

static gboolean
diff_dirs (GFile          *a,
           GFile          *b,
           GPtrArray      *modified,
           GPtrArray      *removed,
           GPtrArray      *added,
           GCancellable   *cancellable,
           GError        **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *dir_enum = NULL;
  GError *temp_error = NULL;
  GFile *child_a = NULL;
  GFile *child_b = NULL;
  GFileInfo *child_a_info = NULL;
  GFileInfo *child_b_info = NULL;

  dir_enum = g_file_enumerate_children (a, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_a_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      GFileType child_a_type;
      GFileType child_b_type;

      name = g_file_info_get_name (child_a_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);
      child_a_type = g_file_info_get_file_type (child_a_info);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_b_info);
      child_b_info = g_file_query_info (child_b, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_b_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (removed, g_object_ref (child_a));
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          child_b_type = g_file_info_get_file_type (child_b_info);
          if (child_a_type != child_b_type)
            {
              OstreeRepoDiffItem *diff_item = diff_item_new (child_a, child_a_info,
                                                             child_b, child_b_info, NULL, NULL);
              
              g_ptr_array_add (modified, diff_item);
            }
          else
            {
              OstreeRepoDiffItem *diff_item = NULL;

              if (!diff_files (child_a, child_a_info, child_b, child_b_info, &diff_item, cancellable, error))
                goto out;
              
              if (diff_item)
                g_ptr_array_add (modified, diff_item); /* Transfer ownership */

              if (child_a_type == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_dirs (child_a, child_b, modified,
                                  removed, added, cancellable, error))
                    goto out;
                }
            }
        }
      
      g_clear_object (&child_a_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_clear_object (&dir_enum);
  dir_enum = g_file_enumerate_children (b, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_b_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_b_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_a_info);
      child_a_info = g_file_query_info (child_a, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_a_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (added, g_object_ref (child_b));
              if (g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_add_dir_recurse (child_b, added, cancellable, error))
                    goto out;
                }
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&child_a_info);
  g_clear_object (&child_b_info);
  g_clear_object (&child_a);
  g_clear_object (&child_b);
  return ret;
}

gboolean
ostree_repo_read_commit (OstreeRepo *self,
                         const char *rev, 
                         GFile       **out_root,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  GFile *ret_root = NULL;
  char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = _ostree_repo_file_new_root (self, resolved_rev);
  if (!_ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, ret_root);
 out:
  g_free (resolved_rev);
  g_clear_object (&ret_root);
  return ret;
}
                       
gboolean
ostree_repo_diff (OstreeRepo     *self,
                  GFile          *src,
                  GFile          *target,
                  GPtrArray     **out_modified,
                  GPtrArray     **out_removed,
                  GPtrArray     **out_added,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *ret_modified = NULL;
  GPtrArray *ret_removed = NULL;
  GPtrArray *ret_added = NULL;

  ret_modified = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_repo_diff_item_unref);
  ret_removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  ret_added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (!diff_dirs (src, target, ret_modified, ret_removed, ret_added, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_modified, ret_modified);
  ot_transfer_out_value(out_removed, ret_removed);
  ot_transfer_out_value(out_added, ret_added);
 out:
  if (ret_modified)
    g_ptr_array_free (ret_modified, TRUE);
  if (ret_removed)
    g_ptr_array_free (ret_removed, TRUE);
  if (ret_added)
    g_ptr_array_free (ret_added, TRUE);
  return ret;
}
