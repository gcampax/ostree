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
#include "ostree-repo-file.h"

#include <glib/gi18n.h>

static gboolean recursive;
static gboolean checksum;
static gboolean xattrs;
static gboolean opt_nul_filenames_only;

static GOptionEntry options[] = {
  { "recursive", 'R', 0, G_OPTION_ARG_NONE, &recursive, "Print directories recursively", NULL },
  { "checksum", 'C', 0, G_OPTION_ARG_NONE, &checksum, "Print checksum", NULL },
  { "xattrs", 'X', 0, G_OPTION_ARG_NONE, &xattrs, "Print extended attributes", NULL },
  { "nul-filenames-only", 0, 0, G_OPTION_ARG_NONE, &opt_nul_filenames_only, "Print only filenames, NUL separated", NULL },
  { NULL }
};

static void
print_one_file_text (GFile     *f,
                     GFileInfo *file_info)
{
  GString *buf = NULL;
  char type_c;
  guint32 mode;
  guint32 type;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)f, NULL))
    g_assert_not_reached ();
  
  buf = g_string_new ("");

  type_c = '?';
  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  type = g_file_info_get_file_type (file_info);
  switch (type)
    {
    case G_FILE_TYPE_REGULAR:
      type_c = '-';
      break;
    case G_FILE_TYPE_DIRECTORY:
      type_c = 'd';
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      type_c = 'l';
      break;
    case G_FILE_TYPE_SPECIAL:
      if (S_ISCHR(mode))
        type_c = 'c';
      else if (S_ISBLK(mode))
        type_c = 'b';
      break;
    case G_FILE_TYPE_UNKNOWN:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
      g_assert_not_reached ();
      break;
    }
  g_string_append_c (buf, type_c);
  g_string_append_printf (buf, "0%04o %u %u %6" G_GUINT64_FORMAT " ",
                          mode & ~S_IFMT,
                          g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                          g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                          g_file_info_get_attribute_uint64 (file_info, "standard::size"));
  
  if (checksum)
    {
      if (type == G_FILE_TYPE_DIRECTORY)
        g_string_append_printf (buf, "%s ", ostree_repo_file_tree_get_content_checksum ((OstreeRepoFile*)f));
      g_string_append_printf (buf, "%s ", ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }

  if (xattrs)
    {
      GVariant *xattrs;
      char *formatted;

      if (!ostree_repo_file_get_xattrs ((OstreeRepoFile*)f, &xattrs, NULL, NULL))
        g_assert_not_reached ();
      
      formatted = g_variant_print (xattrs, TRUE);
      g_string_append (buf, "{ ");
      g_string_append (buf, formatted);
      g_string_append (buf, " } ");
      g_free (formatted);
      g_variant_unref (xattrs);
    }

  g_string_append (buf, ot_gfile_get_path_cached (f));

  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    g_string_append_printf (buf, " -> %s", g_file_info_get_attribute_byte_string (file_info, "standard::symlink-target"));
      
  g_print ("%s\n", buf->str);

  g_string_free (buf, TRUE);
}

static void
print_one_file_binary (GFile     *f,
                       GFileInfo *file_info)
{
  const char *path;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)f, NULL))
    g_assert_not_reached ();

  path = ot_gfile_get_path_cached (f);

  fwrite (path, 1, strlen (path), stdout);
  fwrite ("\0", 1, 1, stdout);
}

static void
print_one_file (GFile     *f,
                GFileInfo *file_info)
{
  if (opt_nul_filenames_only)
    print_one_file_binary (f, file_info);
  else
    print_one_file_text (f, file_info);
}

static gboolean
print_directory_recurse (GFile    *f,
                         GError  **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFileInfo *child_info = NULL;
  GError *temp_error = NULL;

  dir_enum = g_file_enumerate_children (f, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, 
                                        error);
  if (!dir_enum)
    goto out;
  
  while ((child_info = g_file_enumerator_next_file (dir_enum, NULL, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (f, g_file_info_get_name (child_info));

      print_one_file (child, child_info);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!print_directory_recurse (child, error))
            goto out;
        }

      g_clear_object (&child_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_ls (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  ot_lobj OstreeRepo *repo = NULL;
  const char *rev;
  int i;
  ot_lobj GFile *root = NULL;
  ot_lobj GFile *f = NULL;
  ot_lobj GFileInfo *file_info = NULL;

  context = g_option_context_new ("COMMIT PATH [PATH...] - List file paths");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc <= 2)
    {
      ot_util_usage_error (context, "An COMMIT and at least one PATH argument are required", error);
      goto out;
    }
  rev = argv[1];

  if (!ostree_repo_read_commit (repo, rev, &root, NULL, error))
    goto out;

  for (i = 2; i < argc; i++)
    {
      g_clear_object (&f);
      f = g_file_resolve_relative_path (root, argv[i]);

      g_clear_object (&file_info);
      file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     NULL, error);
      if (!file_info)
        goto out;
      
      print_one_file (f, file_info);

      if (recursive && g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!print_directory_recurse (f, error))
            goto out;
        }
    }
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
