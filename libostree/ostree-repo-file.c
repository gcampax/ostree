/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "otutil.h"

static void ot_repo_file_file_iface_init (GFileIface *iface);

struct _OstreeRepoFile
{
  GObject parent_instance;

  OstreeRepo *repo;

  char *commit;
  char *path;
  const char *basename;

  GError *resolve_error;
  OstreeRepoFile *parent;

  GVariant *tree_contents;
  GVariant *tree_metadata;
  GFile *local_file;
  char *file_checksum;

  gboolean resolved : 1;
  gboolean is_tree : 1;
};

#define ot_repo_file_get_type _ot_repo_file_get_type
G_DEFINE_TYPE_WITH_CODE (OstreeRepoFile, ot_repo_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						ot_repo_file_file_iface_init))

static void
ostree_repo_file_finalize (GObject *object)
{
  OstreeRepoFile *self;

  local = OSTREE_REPO_FILE (object);

  g_clear_error (&self->resolve_error);
  if (self->tree_contents)
    g_variant_unref (self->tree_contents);
  if (self->tree_metadata)
    g_variant_unref (self->tree_metadata);
  g_free (self->commit);
  g_free (self->path);

  G_OBJECT_CLASS (ostree_repo_file_parent_class)->finalize (object);
}

static void
ostree_repo_file_dispose (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->parent);
  g_clear_object (&self->local_file);

  if (G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose)
    G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose (object);
}

static void
ostree_repo_file_class_init (OstreeRepoFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileAttributeInfoList *list;

  gobject_class->finalize = ostree_repo_file_finalize;
  gobject_class->dispose = ostree_repo_file_dispose;
}

static void
ostree_repo_file_init (OstreeRepoFile *self)
{
}

GFile * 
_ostree_repo_file_new (OstreeRepo  *repo,
		       const char  *commit,
		       const char  *path)
{
  OstreeRepoFile *self;

  g_return_val_if_fail (repo != NULL, NULL);
  g_return_val_if_fail (commit != NULL, NULL);
  g_return_val_if_fail (strlen (commit) == 64, NULL);
  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (*path == '/') != NULL, NULL);

  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (repo);
  self->commit = g_strdup (commit);
  self->path = g_strdup (path);
  self->basename = strrchr (self->path, '/');
  g_assert (self->basename);
  self->basename += 1;

  return G_FILE (self);
}

static gboolean
do_resolve_commit (OstreeRepoFile  *self)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  GVariant *root_contents = NULL;
  GVariant *root_metadata = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;

  g_assert (self->parent == NULL);

  if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_COMMIT_VARIANT,
                                         self->commit, &commit, &self->resolve_error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (ret_commit, 6, "&s", &tree_contents_checksum);
  g_variant_get_child (ret_commit, 7, "&s", &tree_meta_checksum);

  if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_TREE_VARIANT,
                                         tree_contents_checksum, &root_contents,
					 &self->resolve_error))
    goto out;

  if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                         tree_meta_checksum, &root_metadata,
					 &self->resolve_error))
    goto out;
  
  self->tree_metadata = root_metadata;
  root_metadata = NULL;
  self->tree_contents = root_contents;
  root_contents = NULL;

  self->is_tree = TRUE;
  
 out:
  if (commit)
    g_variant_unref (commit);
  if (root_metadata)
    g_variant_unref (root_metadata);
  if (root_contents)
    g_variant_unref (root_contents);
  return ret;
}

static gboolean
do_resolve_from_parent (OstreeRepoFile   *self)
{
  gboolean ret = FALSE;
  GVariant *parent_tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  GVariant *tree_contents = NULL;
  GVariant *tree_metadata = NULL;
  int i, n;

  g_assert (self->parent != NULL);

  parent_tree = _ostree_repo_file_tree_get_contents (self->parent);

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (parent_tree, 2);
  dirs_variant = g_variant_get_child_value (parent_tree, 3);

  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      const char *checksum;

      g_variant_get_child (files_variant, i, "(&s&s)", &filename, &checksum);

      if (strcmp (filename, self->basename) == 0)
	{
	  char *local_path;
	  char *relpath;
	  
	  relpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE,
						     ostree_repo_is_archive (self->repo));
	  local_path = g_build_filename (ostree_repo_get_path (self->repo), relpath);
	  g_free (relpath);
	  self->local_file = ot_util_new_file_for_path (local_path);
	  g_free (local_path);

	  self->file_checksum = g_strdup (checksum);
	  ret = TRUE;
	  goto out;
	}	
    }

  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      const char *tree_checksum;
      const char *meta_checksum;

      g_variant_get_child (dirs_variant, i, "(&s&s&s)",
                           &dirname, &tree_checksum, &meta_checksum);

      if (strcmp (dirname, self->basename) == 0)
	{
	  if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_TREE_VARIANT,
						 tree_contents_checksum, &tree_contents,
						 &self->resolve_error))
	    goto out;
	  
	  if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_DIRMETA_VARIANT,
						 tree_meta_checksum, &tree_metadata,
						 &self->resolve_error))
	    goto out;


	  self->tree_contents = tree_contents;
	  tree_contents = NULL;
	  self->tree_metadata = tree_metadata;
	  tree_metadata = NULL;
	  ret = TRUE;
	  goto out;
	}
    }
  
  ret = FALSE;
  g_set_error (&self->resolve_error,
	       G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
	       "No such file or directory: %s",
	       self->path);

 out:
  /* parent_tree is const */
  if (files_variant)
    g_variant_unref (files_variant);
  if (dirs_variant)
    g_variant_unref (dirs_variant);
  if (tree_metadata)
    g_variant_unref (tree_metadata);
  if (tree_contents)
    g_variant_unref (tree_contents);
  return ret;
}

gboolean
ostree_repo_file_ensure_resolved (OstreeRepoFile  *self,
				  GError         **error)
{
  gboolean ret = FALSE;

  if (self->resolved)
    {
      ret = self->resolve_error == NULL;
      goto out;
    }

  self->parent = g_file_get_parent (G_FILE (self));

  if (self->parent == NULL)
    {
      if (!do_resolve_commit (self))
	goto out;
    }
  else
    {
      if (!do_resolve_from_parent (self))
	goto out;
    }
  
  ret = TRUE;
 out:
  self->resolved = TRUE;
  if (self->resolve_error)
    {
      if (error)
	*error = g_error_copy (self->resolve_error);
      return FALSE;
    }
  else
    return TRUE;
  return ret;
}

GVariant *
_ostree_repo_file_tree_get_contents (OstreeRepoFile  *self)
{
  g_assert (self->resolved);
  g_assert (self->is_tree);
  
  return self->tree_contents;
}

GVariant *
_ostree_repo_file_tree_get_metadata (OstreeRepoFile  *self)
{
  g_assert (self->resolved);
  g_assert (self->is_tree);
  
  return self->tree_metadata;
}

const char *
_ostree_repo_file_get_relpath (OstreeRepoFile  *self)
{
  return self->path;
}

GFile*
_ostree_repo_file_nontree_get_local (OstreeRepoFile  *self)
{
  g_assert (self->resolved);
  g_assert (!self->is_tree);
  
  return self->local_file;
}

static gboolean
ostree_repo_file_is_native (GFile *file)
{
  return FALSE;
}

static gboolean
ostree_repo_file_has_uri_scheme (GFile      *file,
				 const char *uri_scheme)
{
  return g_ascii_strcasecmp (uri_scheme, "ostree") == 0;
}

static char *
ostree_repo_file_get_uri_scheme (GFile *file)
{
  return g_strdup ("ostree");
}

static char *
ostree_repo_file_get_basename (GFile *file)
{
  return g_file_get_basename (OSTREE_REPO_FILE (file)->path);
}

static char *
ostree_repo_file_get_path (GFile *file)
{
  return g_strdup (OSTREE_REPO_FILE (file)->path);
}

static char *
ostree_repo_file_get_uri (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  char *uri_path;
  char *ret;

  uri_path = g_escape_uri_string (self->path, UNSAFE_PATH);
  ret = g_strconcat ("ostree://", self->commit, "/", uri_path, NULL);
  g_free (uri_path);

  return ret;
}

static char *
ostree_repo_file_get_parse_name (GFile *file)
{
  return ostree_repo_file_get_uri (file);
}

static GFile *
ostree_repo_file_get_parent (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  const char *non_root;
  char *dirname;
  GFile *parent;

  /* Check for root */
  non_root = g_path_skip_root (self->path);
  if (*non_root == 0)
    return NULL;

  dirname = g_path_get_dirname (self->filename);
  parent = _ostree_repo_file_new (self->repo, self->commit, self->dirname);
  g_free (dirname);
  return parent;
}

static GFile *
ostree_repo_file_dup (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  return _ostree_repo_file_new (self->repo, self->commit, self->path);
}

static guint
ostree_repo_file_hash (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  
  return g_str_hash (self->commit) + g_str_hash (self->path);
}

static gboolean
ostree_repo_file_equal (GFile *file1,
		    GFile *file2)
{
  OstreeRepoFile *self1 = OSTREE_REPO_FILE (file1);
  OstreeRepoFile *self2 = OSTREE_REPO_FILE (file2);

  return g_str_equal (self1->commit, local2->commit)
    && g_str_equal (self1->path, self2->path);
}

static const char *
match_prefix (const char *path, 
              const char *prefix)
{
  int prefix_len;

  prefix_len = strlen (prefix);
  if (strncmp (path, prefix, prefix_len) != 0)
    return NULL;
  
  /* Handle the case where prefix is the root, so that
   * the IS_DIR_SEPRARATOR check below works */
  if (prefix_len > 0 &&
      G_IS_DIR_SEPARATOR (prefix[prefix_len-1]))
    prefix_len--;
  
  return path + prefix_len;
}

static gboolean
ostree_repo_file_prefix_matches (GFile *parent,
				 GFile *descendant)
{
  OstreeRepoFile *parent_self = OSTREE_REPO_FILE (parent);
  OstreeRepoFile *descendant_self = OSTREE_REPO_FILE (descendant);
  const char *remainder;

  remainder = match_prefix (descendant_self->path, parent_self->path);
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return TRUE;
  return FALSE;
}

static char *
ostree_repo_file_get_relative_path (GFile *parent,
				    GFile *descendant)
{
  OstreeRepoFile *parent_self = OSTREE_REPO_FILE (parent);
  OstreeRepoFile *descendant_self = OSTREE_REPO_FILE (descendant);
  const char *remainder;

  remainder = match_prefix (descendant_self->path, parent_self->path);
  
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return g_strdup (remainder + 1);
  return NULL;
}

static GFile *
ostree_repo_file_resolve_relative_path (GFile      *file,
					const char *relative_path)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  char *filename;
  GFile *child;

  if (g_path_is_absolute (relative_path))
    return _ostree_repo_file_new (self->repo, 
				  self->commit,
				  relative_path);
  
  filename = g_build_filename (self->path, relative_path, NULL);
  child = _ostree_repo_file_new (self->repo, self->commit,
				 filename);
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
ostree_repo_file_enumerate_children (GFile                *file,
				     const char           *attributes,
				     GFileQueryInfoFlags   flags,
				     GCancellable         *cancellable,
				     GError              **error)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  return _ostree_repo_file_enumerator_new (self,
					   attributes, flags,
					   cancellable, error);
}

static GFile *
ostree_repo_file_get_child_for_display_name (GFile        *file,
					 const char   *display_name,
					 GError      **error)
{
  return g_file_get_child (file, display_name);
}

void
query_info_common (OstreeRepoFile  *self,
		   GFileInfo       *info)
{
  if (*(self->basename) == '.')
    g_file_info_set_is_hidden (info, TRUE);

  g_file_info_set_attribute_string (info, "standard::name",
				    self->basename);
  g_file_info_set_attribute_string (info, "standard::display-name",
				    self->basename);
}

static gboolean
query_info_file_nonarchive (OstreeRepoFile   *self,
			    GFileInfo        *info,
			    GCancellable     *cancellable,
			    GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *local_info = NULL;
  int i ;
  const char mapped_boolean[] = {
    "standard::is-symlink"
  };
  const char mapped_string[] = {
  };
  const char mapped_byte_string[] = {
    "standard::symlink-target"
  };
  const char mapped_uint32[] = {
    "standard::type",
    "unix::device",
    "unix::mode",
    "unix::nlink",
    "unix::uid",
    "unix::gid",
    "unix::rdev"
  };
  const char mapped_uint64[] = {
    "standard::size",
    "standard::allocated-size",
    "unix::inode"
  };

  local_info = g_file_query_info (self->local_file,
				  OSTREE_GIO_FAST_QUERYINFO,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable,
				  error);
  if (!local_info)
    goto out;

  for (i = 0; i < G_N_ELEMENTS (mapped_boolean); i++)
    g_file_info_set_attribute_boolean (info, g_file_info_get_attribute_boolean (local_info));

  for (i = 0; i < G_N_ELEMENTS (mapped_string); i++)
    g_file_info_set_attribute_string (info, g_file_info_get_attribute_string (local_info));

  for (i = 0; i < G_N_ELEMENTS (mapped_byte_string); i++)
    g_file_info_set_attribute_byte_string (info, g_file_info_get_attribute_byte_string (local_info));

  for (i = 0; i < G_N_ELEMENTS (mapped_uint32); i++)
    g_file_info_set_attribute_uint32 (info, g_file_info_get_attribute_uint32 (local_info));

  for (i = 0; i < G_N_ELEMENTS (mapped_uint64); i++)
    g_file_info_set_attribute_uint64 (info, g_file_info_get_attribute_uint64 (local_info));
  
  ret = TRUE;
 out:
  g_clear_object (&local_info);
  return ret;
}

static GFileInfo *
ostree_repo_file_query_info (GFile                *file,
			     const char           *attributes,
			     GFileQueryInfoFlags   flags,
			     GCancellable         *cancellable,
			     GError              **error)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  GFileInfo *info;

  if (!_ostree_repo_file_ensure_resolved (self, error))
    return NULL;

  info = g_file_info_new ();

  query_info_common (self, info);

  if (self->local_file)
    {
      if (ostree_repo_is_archive (self->repo))
	{
	}
      else
	{
	  if (!query_info_file_nonarchive (self, info, error))
	    goto out;
	}
    }
  else
    {
      uint32 uid;
      uint32 gid;
      uint32 mode;

      g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_DIRECTORY);

      g_variant_get_child (self->tree_metadata, 1, "u", &uid);
      uid = GUINT32_FROM_BE (uid);
      g_file_info_set_attribute_uint32 (info, "unix::uid", uid);

      g_variant_get_child (self->tree_metadata, 2, "u", &gid);
      gid = GUINT32_FROM_BE (gid);
      g_file_info_set_attribute_uint32 (info, "unix::gid", gid);

      g_variant_get_child (self->tree_metadata, 3, "u", &mode);
      mode = GUINT32_FROM_BE (mode);
      g_file_info_set_attribute_uint32 (info, "unix::mode", mode);
    }

  return info;
}

static GFileAttributeInfoList *
ostree_repo_file_query_settable_attributes (GFile         *file,
					GCancellable  *cancellable,
					GError       **error)
{
  return g_file_attribute_info_list_new ();
}

static GFileAttributeInfoList *
ostree_repo_file_query_writable_namespaces (GFile         *file,
					GCancellable  *cancellable,
					GError       **error)
{
  return g_file_attribute_info_list_new ();
}

static GFileInputStream *
ostree_repo_file_read (GFile         *file,
		       GCancellable  *cancellable,
		       GError       **error)
{
  gboolean ret = FALSE;
  GFileInputStream *ret_stream = NULL;
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  if (!_ostree_repo_file_ensure_resolved (self, error))
    goto out;
  
  if (self->is_tree)
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_IS_DIRECTORY,
			   _("Can't open directory"));
      goto out;
    }

  if (ostree_repo_is_archive (self->repo))
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_NOT_SUPPORTED,
			   _("Can't open archived file (yet)"));
      goto out;
    }
  else
    {
      ret_stream = g_file_read (self->local_file, cancellable, error);
      if (!ret_stream)
	goto out;
    }
  
  ret = TRUE;
 out:
  if (!ret)
    g_clear_object (&ret_stream);
  return ret_stream;
}

static void
ostree_repo_file_file_iface_init (GFileIface *iface)
{
  iface->dup = ostree_repo_file_dup;
  iface->hash = ostree_repo_file_hash;
  iface->equal = ostree_repo_file_equal;
  iface->is_native = ostree_repo_file_is_native;
  iface->has_uri_scheme = ostree_repo_file_has_uri_scheme;
  iface->get_uri_scheme = ostree_repo_file_get_uri_scheme;
  iface->get_basename = ostree_repo_file_get_basename;
  iface->get_path = ostree_repo_file_get_path;
  iface->get_uri = ostree_repo_file_get_uri;
  iface->get_parse_name = ostree_repo_file_get_parse_name;
  iface->get_parent = ostree_repo_file_get_parent;
  iface->prefix_matches = ostree_repo_file_prefix_matches;
  iface->get_relative_path = ostree_repo_file_get_relative_path;
  iface->resolve_relative_path = ostree_repo_file_resolve_relative_path;
  iface->get_child_for_display_name = ostree_repo_file_get_child_for_display_name;
  iface->set_display_name = NULL;
  iface->enumerate_children = ostree_repo_file_enumerate_children;
  iface->query_info = ostree_repo_file_query_info;
  iface->query_filesystem_info = ostree_repo_file_query_filesystem_info;
  iface->find_enclosing_mount = ostree_repo_file_find_enclosing_mount;
  iface->query_settable_attributes = ostree_repo_file_query_settable_attributes;
  iface->query_writable_namespaces = ostree_repo_file_query_writable_namespaces;
  iface->set_attribute = NULL;
  iface->set_attributes_from_info = NULL;
  iface->read_fn = ostree_repo_file_read;
  iface->append_to = NULL;
  iface->create = NULL;
  iface->replace = NULL;
  iface->open_readwrite = NULL;
  iface->create_readwrite = NULL;
  iface->replace_readwrite = NULL;
  iface->delete_file = NULL;
  iface->trash = NULL;
  iface->make_directory = NULL;
  iface->make_symbolic_link = NULL;
  iface->copy = NULL;
  iface->move = NULL;
  iface->monitor_dir = NULL;
  iface->monitor_file = NULL;

  iface->supports_thread_contexts = TRUE;
}

const char *
_ostree_repo_file_get_checksum (OstreeRepoFile  *self)
{
  return self->checksum;
}
