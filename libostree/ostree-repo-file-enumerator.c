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
#include <string.h>

struct _OstreeRepoFileEnumerator
{
  GFileEnumerator parent;

  OstreeRepoFile *dir;
  GFileAttributeMatcher *matcher;
  GFileQueryInfoFlags flags;

  int index;
};

#define ostree_repo_file_enumerator_get_type _ostree_repo_file_enumerator_get_type
G_DEFINE_TYPE (OstreeRepoFileEnumerator, ostree_repo_file_enumerator, OSTREE_TYPE_REPO_FILE_ENUMERATOR);

static GFileInfo *ostree_repo_file_enumerator_next_file (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);
static gboolean   ostree_repo_file_enumerator_close     (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);


static void
ostree_repo_file_enumerator_dispose (GObject *object)
{
  OstreeRepoFileEnumerator *self;

  self = OSTREE_REPO_FILE_ENUMERATOR (object);

  g_clear_object (&self->dir);
  g_file_attribute_matcher_unref (self->matcher);
  
  if (G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->dispose)
    G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->dispose (object);
}

static void
ostree_repo_file_enumerator_finalize (GObject *object)
{
  OstreeRepoFileEnumerator *self;

  self = OSTREE_REPO_FILE_ENUMERATOR (object);
  (void)self;

  G_OBJECT_CLASS (ostree_repo_file_enumerator_parent_class)->finalize (object);
}


static void
ostree_repo_file_enumerator_class_init (OstreeRepoFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = ostree_repo_file_enumerator_finalize;
  gobject_class->dispose = ostree_repo_file_enumerator_dispose;

  enumerator_class->next_file = ostree_repo_file_enumerator_next_file;
  enumerator_class->close_fn = ostree_repo_file_enumerator_close;
}

static void
ostree_repo_file_enumerator_init (OstreeRepoFileEnumerator *self)
{
}

GFileEnumerator *
_ostree_repo_file_enumerator_new (OstreeRepoFile       *dir,
				  const char           *attributes,
				  GFileQueryInfoFlags   flags,
				  GCancellable         *cancellable,
				  GError              **error)
{
  OstreeRepoFileEnumerator *self;
  
  self = g_object_new (G_TYPE_LOCAL_FILE_ENUMERATOR,
		       "container", dir,
		       NULL);

  self->dir = g_object_ref (dir);
  self->matcher = g_file_attribute_matcher_new (attributes);
  self->flags = flags;
  
  return G_FILE_ENUMERATOR (local);
}

static GFileInfo *
ostree_repo_file_enumerator_next_file (GFileEnumerator  *enumerator,
				       GCancellable     *cancellable,
				       GError          **error)
{
  OstreeRepoFileEnumerator *self = OSTREE_REPO_FILE_ENUMERATOR (enumerator);
  gboolean ret = FALSE;
  GFileInfo *info = NULL;
  GFile *temp_child = NULL;
  GVariant *tree_contents = NULL;
  int n;
  const char *name;
  const char *checksum;

  tree_contents = _ostree_repo_file_tree_get_contents (self->dir);
  n = g_variant_n_children (tree_contents);

  g_variant_get_child (

  if (self->i >= n)
    {
      ret = TRUE;
      goto out;
    }

  temp_child = g_file
  if (

 out:
 g_clear_object (&temp_child);
  if (!ret)
    g_clear_object (&info);
  return info;
}

static gboolean
ostree_repo_file_enumerator_close (GFileEnumerator  *enumerator,
				   GCancellable     *cancellable,
				   GError          **error)
{
  OstreeRepoFileEnumerator *self = OSTREE_REPO_FILE_ENUMERATOR (enumerator);

  if (self->dir)
    {
#ifdef USE_GDIR
      g_dir_close (self->dir);
#else
      closedir (self->dir);
#endif
      self->dir = NULL;
    }

  return TRUE;
}
