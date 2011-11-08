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

#include "ostree-repo-file-enumerator.h"

static void ostree_repo_file_file_iface_init (GFileIface *iface);

struct _OstreeRepoFile
{
  GObject parent_instance;

  OstreeRepo *repo;

  char *commit;
  GError *commit_resolve_error;
  
  OstreeRepoFile *parent;
  char *name;

  GVariant *tree_contents;
  GVariant *tree_metadata;
};

#define ostree_repo_file_get_type _ostree_repo_file_get_type
G_DEFINE_TYPE_WITH_CODE (OstreeRepoFile, ostree_repo_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						ostree_repo_file_file_iface_init))

static void
ostree_repo_file_finalize (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  if (self->tree_contents)
    g_variant_unref (self->tree_contents);
  if (self->tree_metadata)
    g_variant_unref (self->tree_metadata);
  g_free (self->commit);
  g_free (self->name);

  G_OBJECT_CLASS (ostree_repo_file_parent_class)->finalize (object);
}

static void
ostree_repo_file_dispose (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->parent);

  if (G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose)
    G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose (object);
}

static void
ostree_repo_file_class_init (OstreeRepoFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_repo_file_finalize;
  gobject_class->dispose = ostree_repo_file_dispose;
}

static void
ostree_repo_file_init (OstreeRepoFile *self)
{
}

GFile * 
_ostree_repo_file_new_root (OstreeRepo  *repo,
                            const char  *commit)
{
  OstreeRepoFile *self;

  g_return_val_if_fail (repo != NULL, NULL);
  g_return_val_if_fail (commit != NULL, NULL);
  g_return_val_if_fail (strlen (commit) == 64, NULL);

  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (repo);
  self->commit = g_strdup (commit);

  return G_FILE (self);
}


GFile *
_ostree_repo_file_new_child (OstreeRepoFile *parent,
                             const char  *name)
{
  OstreeRepoFile *self;
  
  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (parent->repo);
  self->parent = g_object_ref (parent);
  self->name = g_strdup (name);

  return G_FILE (self);
}

static gboolean
do_resolve_commit (OstreeRepoFile  *self,
                   GError         **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  GVariant *root_contents = NULL;
  GVariant *root_metadata = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;

  g_assert (self->parent == NULL);

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_COMMIT_VARIANT,
                                         self->commit, &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
  g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_TREE_VARIANT,
                                         tree_contents_checksum, &root_contents,
					 error))
    goto out;

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                         tree_meta_checksum, &root_metadata,
					 error))
    goto out;
  
  self->tree_metadata = root_metadata;
  root_metadata = NULL;
  self->tree_contents = root_contents;
  root_contents = NULL;

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
ensure_resolved (OstreeRepoFile  *self,
                 GError         **error)
{
  if (self->parent == NULL
      && self->commit_resolve_error == NULL
      && self->tree_contents == NULL)
    {
      (void)do_resolve_commit (self, &(self->commit_resolve_error));
    }
  
  if (self->commit_resolve_error)
    {
      if (error)
	*error = g_error_copy (self->commit_resolve_error);
      return FALSE;
    }
  else
    return TRUE;
}

GVariant *
_ostree_repo_file_tree_get_contents (OstreeRepoFile  *self)
{
  return self->tree_contents;
}

GVariant *
_ostree_repo_file_tree_get_metadata (OstreeRepoFile  *self)
{
  return self->tree_metadata;
}

GFile *
_ostree_repo_file_nontree_get_local (OstreeRepoFile  *self)
{
  const char *checksum;
  char *path;
  GFile *ret;

  g_assert (!ostree_repo_is_archive (self->repo));

  checksum = _ostree_repo_file_nontree_get_checksum (self);
  path = ostree_repo_get_object_path (self->repo, checksum, OSTREE_OBJECT_TYPE_FILE);
  ret = ot_util_new_file_for_path (path);
  g_free (path);
  
  return ret;
}

OstreeRepo *
_ostree_repo_file_get_repo (OstreeRepoFile  *self)
{
  return self->repo;
}

OstreeRepoFile *
_ostree_repo_file_get_root (OstreeRepoFile  *self)
{
  OstreeRepoFile *parent = self;

  while (parent->parent)
    parent = parent->parent;
  return parent;
}

const char *
_ostree_repo_file_nontree_get_checksum (OstreeRepoFile  *self)
{
  int n;

  g_assert (self->parent);

  n = _ostree_repo_file_tree_find_child (self->parent, self->name);
  g_assert (n >= 0);
  
  return _ostree_repo_file_tree_get_child_checksum (self->parent, n);
}

const char *
_ostree_repo_file_tree_get_child_checksum (OstreeRepoFile  *self,
                                           int n)
{
  GVariant *files_variant;
  const char *checksum;

  g_assert (self->tree_contents);

  files_variant = g_variant_get_child_value (self->tree_contents, 2);

  g_variant_get_child (files_variant, n, "(@s&s)", NULL, &checksum);

  g_variant_unref (files_variant);

  return checksum;
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
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  return g_strdup (self->name);
}

static char *
ostree_repo_file_get_path (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  OstreeRepoFile *parent;
  GString *buf;
  GSList *parents;
  GSList *iter;

  buf = g_string_new ("");
  parents = NULL;

  for (parent = self->parent; parent; parent = parent->parent)
    parents = g_slist_prepend (parents, parent);

  if (parents->next)
    {
      for (iter = parents->next; iter; iter = iter->next)
        {
          parent = iter->data;
          g_string_append_c (buf, '/');
          g_string_append (buf, parent->name);
        }
    }
  g_string_append_c (buf, '/');
  g_string_append (buf, self->name);

  g_slist_free (parents);

  return g_string_free (buf, FALSE);
}

static char *
ostree_repo_file_get_uri (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  char *path;
  char *uri_path;
  char *ret;

  path = g_file_get_path (file);
  uri_path = g_filename_to_uri (path, NULL, NULL);
  g_free (path);
  g_assert (g_str_has_prefix (uri_path, "file://"));
  ret = g_strconcat ("ostree://", self->commit, uri_path+strlen("file://"), NULL);
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

  return g_object_ref (self->parent);
}

static GFile *
ostree_repo_file_dup (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  if (self->parent)
    return _ostree_repo_file_new_child (self->parent, self->name);
  else
    return _ostree_repo_file_new_root (self->repo, self->commit);
}

static guint
ostree_repo_file_hash (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  
  if (self->parent)
    return g_file_hash (self->parent) + g_str_hash (self->name);
  else
    return g_str_hash (self->commit);
}

static gboolean
ostree_repo_file_equal (GFile *file1,
                        GFile *file2)
{
  OstreeRepoFile *self1 = OSTREE_REPO_FILE (file1);
  OstreeRepoFile *self2 = OSTREE_REPO_FILE (file2);

  if (self1->parent && self2->parent)
    {
      return g_str_equal (self1->name, self2->name)
        && g_file_equal ((GFile*)self1->parent, (GFile*)self2->parent);
    }
  else if (!self1->parent && !self2->parent)
    {
      return g_str_equal (self1->commit, self2->commit);
    }
  else
    return FALSE;
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
  const char *remainder;
  char *parent_path;
  char *descendant_path;

  parent_path = g_file_get_path (parent);
  descendant_path = g_file_get_path (descendant);
  remainder = match_prefix (descendant_path, parent_path);
  g_free (parent_path);
  g_free (descendant_path);
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return TRUE;
  return FALSE;
}

static char *
ostree_repo_file_get_relative_path (GFile *parent,
				    GFile *descendant)
{
  const char *remainder;
  char *parent_path;
  char *descendant_path;

  parent_path = g_file_get_path (parent);
  descendant_path = g_file_get_path (descendant);
  remainder = match_prefix (descendant_path, parent_path);
  g_free (parent_path);
  g_free (descendant_path);
  
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return g_strdup (remainder + 1);
  return NULL;
}

static GFile *
ostree_repo_file_resolve_relative_path (GFile      *file,
					const char *relative_path)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  OstreeRepoFile *parent;
  char *filename;
  const char *rest;
  GFile *ret;

  if (g_path_is_absolute (relative_path) && self->parent)
    return ostree_repo_file_resolve_relative_path ((GFile*)_ostree_repo_file_get_root (self),
                                                   relative_path+1);
  rest = strchr (relative_path, '/');
  if (rest)
    rest += 1;
  filename = g_strndup (relative_path, rest - relative_path);
  parent = (OstreeRepoFile*)_ostree_repo_file_new_child (self, filename);
  g_free (filename);
    
  if (!*rest)
    ret = (GFile*)parent;
  else
    {
      ret = ostree_repo_file_resolve_relative_path ((GFile*)parent, rest);
      g_clear_object (&parent);
    }
  return ret;
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

static GFile *
get_child_local_file (OstreeRepo   *repo,
                      const char   *checksum)
{
  char *path;
  GFile *ret;

  path = ostree_repo_get_object_path (repo, checksum, OSTREE_OBJECT_TYPE_FILE);
  ret = ot_util_new_file_for_path (path);
  g_free (path);
  
  return ret;
}

static gboolean
query_child_info_file_nonarchive (OstreeRepo       *repo,
                                  const char       *checksum,
                                  GFileAttributeMatcher *matcher,
                                  GFileInfo        *info,
                                  GCancellable     *cancellable,
                                  GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *local_info = NULL;
  GFile *local_file = NULL;
  int i ;
  const char *mapped_boolean[] = {
    "standard::is-symlink"
  };
  const char *mapped_string[] = {
  };
  const char *mapped_byte_string[] = {
    "standard::symlink-target"
  };
  const char *mapped_uint32[] = {
    "standard::type",
    "unix::device",
    "unix::mode",
    "unix::nlink",
    "unix::uid",
    "unix::gid",
    "unix::rdev"
  };
  const char *mapped_uint64[] = {
    "standard::size",
    "standard::allocated-size",
    "unix::inode"
  };

  if (!(g_file_attribute_matcher_matches (matcher, "unix::mode")
        || g_file_attribute_matcher_matches (matcher, "standard::type")))
    {
      ret = TRUE;
      goto out;
    }

  local_file = get_child_local_file (repo, checksum);
  local_info = g_file_query_info (local_file,
				  OSTREE_GIO_FAST_QUERYINFO,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable,
				  error);
  if (!local_info)
    goto out;

  for (i = 0; i < G_N_ELEMENTS (mapped_boolean); i++)
    g_file_info_set_attribute_boolean (info, mapped_boolean[i], g_file_info_get_attribute_boolean (local_info, mapped_boolean[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_string); i++)
    g_file_info_set_attribute_string (info, mapped_string[i], g_file_info_get_attribute_string (local_info, mapped_string[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_byte_string); i++)
    g_file_info_set_attribute_byte_string (info, mapped_byte_string[i], g_file_info_get_attribute_byte_string (local_info, mapped_byte_string[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_uint32); i++)
    g_file_info_set_attribute_uint32 (info, mapped_uint32[i], g_file_info_get_attribute_uint32 (local_info, mapped_uint32[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_uint64); i++)
    g_file_info_set_attribute_uint64 (info, mapped_uint64[i], g_file_info_get_attribute_uint64 (local_info, mapped_uint64[i]));
  
  ret = TRUE;
 out:
  g_clear_object (&local_info);
  g_clear_object (&local_file);
  return ret;
}

static gboolean
query_child_info_file_archive (OstreeRepo       *repo,
                               const char       *checksum,
                               GFileAttributeMatcher *matcher,
                               GFileInfo        *info,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean ret = FALSE;
  GFile *local_file = NULL;
  GVariant *metadata = NULL;
  GInputStream *input = NULL;
  guint32 version, uid, gid, mode;
  guint64 content_len;
  guint32 file_type;
  gsize bytes_read;
  char *buf = NULL;

  local_file = get_child_local_file (repo, checksum);

  if (!ostree_parse_packed_file (local_file, &metadata, &input, cancellable, error))
    goto out;

  g_variant_get (metadata, "(uuuu@a(ayay)t)",
                 &version, &uid, &gid, &mode,
                 NULL, &content_len);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  content_len = GUINT64_FROM_BE (content_len);

  g_file_info_set_attribute_boolean (info, "standard::is-symlink",
                                     S_ISLNK (mode));
  if (S_ISLNK (mode))
    file_type = G_FILE_TYPE_SYMBOLIC_LINK;
  else if (S_ISREG (mode))
    file_type = G_FILE_TYPE_REGULAR;
  else if (S_ISBLK (mode) || S_ISCHR(mode))
    file_type = G_FILE_TYPE_SPECIAL;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile %s: Invalid mode", checksum);
      goto out;
    }
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);

  g_file_info_set_attribute_uint32 (info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", content_len);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      gsize len = MIN (PATH_MAX, content_len) + 1;
      buf = g_malloc (len);

      if (!g_input_stream_read_all (input, buf, len, &bytes_read, cancellable, error))
        goto out;
      buf[bytes_read] = '\0';

      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", buf);
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      guint32 device;

      if (!g_input_stream_read_all (input, &device, 4, &bytes_read, cancellable, error))
        goto out;

      device = GUINT32_FROM_BE (device);
      g_file_info_set_attribute_uint32 (info, "unix::device", device);
    }

  ret = TRUE;
 out:
  g_free (buf);
  if (metadata)
    g_variant_unref (metadata);
  g_clear_object (&local_file);
  g_clear_object (&input);
  return ret;
}

static void
set_info_from_dirmeta (GFileInfo  *info,
                       GVariant   *metadata)
{
  guint32 version, uid, gid, mode;

  g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_DIRECTORY);

  /* PARSE OSTREE_SERIALIZED_DIRMETA_VARIANT */
  g_variant_get (metadata, "(uuuu@a(ayay))",
                 &version, &uid, &gid, &mode,
                 NULL);
  version = GUINT32_FROM_BE (version);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  g_file_info_set_attribute_uint32 (info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);
}

static gboolean
query_child_info_dir (OstreeRepo         *repo,
                      const char         *metadata_checksum,
                      GFileAttributeMatcher *matcher,
                      GFileQueryInfoFlags flags,
                      GFileInfo        *info,
                      GCancellable    *cancellable,
                      GError         **error)
{
  gboolean ret = FALSE;
  GVariant *metadata = NULL;

  if (!g_file_attribute_matcher_matches (matcher, "unix::mode"))
    {
      ret = TRUE;
      goto out;
    }

  if (!ostree_repo_load_variant_checked (repo, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                         metadata_checksum, &metadata, error))
    goto out;

  set_info_from_dirmeta (info, metadata);
  
  ret = TRUE;
 out:
  if (metadata)
    g_variant_unref (metadata);
  return ret;
}



static int
bsearch_in_file_variant (GVariant  *variant,
                         const char *name)
{
  int i, n;

  i = 0;
  n = g_variant_n_children (variant) - 1;

  while (i <= n)
    {
      int m;
      const char *cur;
      int cmp;

      m = i + ((n - i) / 2);

      g_variant_get_child (variant, m, "(&s@s)", &cur, NULL);

      cmp = strcmp (name, cur);
      if (cmp < 0)
        i = m + 1;
      else if (cmp > 0)
        n = m - 1;
      else
        return m;
    }
  return -1;
}

int
_ostree_repo_file_tree_find_child  (OstreeRepoFile  *self,
                                    const char      *name)
{
  int i;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;

  files_variant = g_variant_get_child_value (self->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  i = bsearch_in_file_variant (files_variant, name);
  if (i < 0)
    i = bsearch_in_file_variant (dirs_variant, name);

  g_variant_unref (files_variant);
  g_variant_unref (dirs_variant);
  return i;
}

gboolean
_ostree_repo_file_tree_query_child (OstreeRepoFile  *self,
                                    int              n,
                                    const char      *attributes,
                                    GFileQueryInfoFlags flags,
                                    GFileInfo      **out_info,
                                    GCancellable    *cancellable,
                                    GError         **error)
{
  const char *name;
  gboolean ret = FALSE;
  GFileInfo *ret_info = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  GVariant *tree_child_metadata = NULL;
  GFileAttributeMatcher *matcher = NULL;
  int c;

  matcher = g_file_attribute_matcher_new (attributes);

  ret_info = g_file_info_new ();

  files_variant = g_variant_get_child_value (self->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  c = g_variant_n_children (files_variant);
  if (n < c)
    {
      const char *checksum;

      g_variant_get_child (files_variant, n, "(&s&s)", &name, &checksum);
     
      if (ostree_repo_is_archive (self->repo))
	{
	  if (!query_child_info_file_archive (self->repo, checksum, matcher, ret_info,
                                              cancellable, error))
	    goto out;
	}
      else
	{
	  if (!query_child_info_file_nonarchive (self->repo, checksum, matcher, ret_info,
                                                 cancellable, error))
	    goto out;
	}
    }
  else
    {
      const char *tree_checksum;
      const char *meta_checksum;

      n -= c;

      c = g_variant_n_children (dirs_variant);

      if (n < c)
        {
          g_variant_get_child (dirs_variant, n, "(&s&s&s)",
                               &name, &tree_checksum, &meta_checksum);
          if (!query_child_info_dir (self->repo, meta_checksum,
                                     matcher, flags, ret_info,
                                     cancellable, error))
            goto out;
        }
    }

  if (!name)
    goto out;

  g_file_info_set_attribute_string (ret_info, "standard::name",
				    name);
  g_file_info_set_attribute_string (ret_info, "standard::display-name",
				    name);
  if (*name == '.')
    g_file_info_set_is_hidden (ret_info, TRUE);

  ret = TRUE;
  *out_info = ret_info;
  ret_info = NULL;
 out:
  g_clear_object (&ret_info);
  if (matcher)
    g_file_attribute_matcher_unref (matcher);
  if (tree_child_metadata)
    g_variant_unref (tree_child_metadata);
  g_variant_unref (files_variant);
  g_variant_unref (dirs_variant);
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
  gboolean ret = FALSE;
  GFileInfo *info = NULL;
  int i;

  if (!self->parent)
    {
      if (!ensure_resolved (self, error))
        goto out;

      info = g_file_info_new ();
      set_info_from_dirmeta (info, self->tree_metadata);
    }
  else
    {
      i = _ostree_repo_file_tree_find_child (self->parent, self->name);

      if (i < 0)
        {
          char *path = g_file_get_path (file);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No such file or directory: %s", path);
          g_free (path);
          goto out;
        }
      
      if (!_ostree_repo_file_tree_query_child (self->parent, i, 
                                               attributes, flags, 
                                               &info, cancellable, error))
        goto out;
    }
      
  ret = TRUE;
 out:
  if (!ret)
    g_clear_object (&info);
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
  GFile *local_file = NULL;
  GFileInputStream *ret_stream = NULL;
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  if (self->tree_contents)
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_IS_DIRECTORY,
			   "Can't open directory");
      goto out;
    }

  if (ostree_repo_is_archive (self->repo))
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_NOT_SUPPORTED,
			   "Can't open archived file (yet)");
      goto out;
    }
  else
    {
      local_file = _ostree_repo_file_nontree_get_local (self);
      ret_stream = g_file_read (local_file, cancellable, error);
      if (!ret_stream)
	goto out;
    }
  
  ret = TRUE;
 out:
  g_clear_object (&local_file);
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
  iface->query_filesystem_info = NULL;
  iface->find_enclosing_mount = NULL;
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
