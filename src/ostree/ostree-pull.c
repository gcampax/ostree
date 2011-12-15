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

#include <libsoup/soup-gnome.h>

#include "ostree.h"
#include "ot-main.h"

gboolean verbose;

static GOptionEntry options[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show more information", NULL },
};

static void
log_verbose (const char  *fmt,
             ...) G_GNUC_PRINTF (1, 2);

static void
log_verbose (const char  *fmt,
             ...)
{
  va_list args;
  char *msg;

  if (!verbose)
    return;

  va_start (args, fmt);
  
  msg = g_strdup_vprintf (fmt, args);
  g_print ("%s\n", msg);
  g_free (msg);
}

static gboolean
fetch_uri (OstreeRepo  *repo,
           SoupSession *soup,
           SoupURI     *uri,
           char       **temp_filename,
           GError     **error)
{
  gboolean ret = FALSE;
  SoupMessage *msg = NULL;
  guint response;
  char *template = NULL;
  int fd;
  SoupBuffer *buf = NULL;
  GFile *tempf = NULL;
  char *uri_string = NULL;
  
  uri_string = soup_uri_to_string (uri, FALSE);
  log_verbose ("Fetching %s", uri_string);
  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);
  
  response = soup_session_send_message (soup, msg);
  if (response != 200)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to retrieve '%s': %d %s",
                   uri_string, response, msg->reason_phrase);
      goto out;
    }

  template = g_strdup_printf ("%s/tmp-fetchXXXXXX", ostree_repo_get_path (repo));
  
  fd = g_mkstemp (template);
  if (fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
  close (fd);
  tempf = ot_gfile_new_for_path (template);

  buf = soup_message_body_flatten (msg->response_body);

  if (!g_file_replace_contents (tempf, buf->data, buf->length, NULL, FALSE, 0, NULL, NULL, error))
    goto out;
  
  *temp_filename = template;
  template = NULL;

  ret = TRUE;
 out:
  g_free (uri_string);
  g_free (template);
  g_clear_object (&msg);
  g_clear_object (&tempf);
  return ret;
}

static gboolean
store_object (OstreeRepo  *repo,
              SoupSession *soup,
              SoupURI     *baseuri,
              const char  *checksum,
              OstreeObjectType objtype,
              gboolean    *did_exist,
              GError     **error)
{
  gboolean ret = FALSE;
  GFile *f = NULL;
  char *filename = NULL;
  char *objpath = NULL;
  char *relpath = NULL;
  SoupURI *obj_uri = NULL;
  GFileInfo *file_info = NULL;
  GInputStream *input = NULL;
  gboolean exists;

  g_assert (objtype != OSTREE_OBJECT_TYPE_RAW_FILE);

  if (!ostree_repo_has_object (repo, objtype, checksum, &exists, NULL, error))
    goto out;

  if (!exists)
    {
      objpath = ostree_get_relative_object_path (checksum, objtype);
      obj_uri = soup_uri_copy (baseuri);
      relpath = g_build_filename (soup_uri_get_path (obj_uri), objpath, NULL);
      soup_uri_set_path (obj_uri, relpath);

      if (!fetch_uri (repo, soup, obj_uri, &filename, error))
        goto out;
      
      f = ot_gfile_new_for_path (filename);

      file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
      if (!file_info)
        goto out;

      input = (GInputStream*)g_file_read (f, NULL, error);
      if (!input)
        goto out;
  
      if (!ostree_repo_store_object (repo, objtype, checksum, file_info, NULL, input, NULL, error))
        goto out;

      *did_exist = FALSE;
    }
  else
    *did_exist = TRUE;

  ret = TRUE;
 out:
  if (obj_uri)
    soup_uri_free (obj_uri);
  if (filename)
    (void) unlink (filename);
  g_clear_object (&f);
  g_clear_object (&file_info);
  g_clear_object (&input);
  g_free (filename);
  g_free (objpath);
  g_free (relpath);
  return ret;
}

static gboolean
store_tree_recurse (OstreeRepo   *repo,
                    SoupSession  *soup,
                    SoupURI      *base_uri,
                    const char   *rev,
                    GError      **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  gboolean did_exist;
  int i, n;

  if (!store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_DIR_TREE, &did_exist, error))
    goto out;

  if (did_exist)
    log_verbose ("Already have tree %s", rev);
  else
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, rev, &tree, error))
        goto out;
      
      /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
      files_variant = g_variant_get_child_value (tree, 2);
      dirs_variant = g_variant_get_child_value (tree, 3);
      
      n = g_variant_n_children (files_variant);
      for (i = 0; i < n; i++)
        {
          const char *filename;
          const char *checksum;

          g_variant_get_child (files_variant, i, "(&s&s)", &filename, &checksum);

          if (!store_object (repo, soup, base_uri, checksum,
                             OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                             &did_exist,
                             error))
            goto out;
        }
      
      n = g_variant_n_children (dirs_variant);
      for (i = 0; i < n; i++)
        {
          const char *dirname;
          const char *tree_checksum;
          const char *meta_checksum;

          g_variant_get_child (dirs_variant, i, "(&s&s&s)",
                               &dirname, &tree_checksum, &meta_checksum);

          if (!store_object (repo, soup, base_uri, meta_checksum, OSTREE_OBJECT_TYPE_DIR_META, &did_exist, error))
            goto out;

          if (!store_tree_recurse (repo, soup, base_uri, tree_checksum, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&tree);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  return ret;
}

static gboolean
store_commit_recurse (OstreeRepo   *repo,
                      SoupSession  *soup,
                      SoupURI      *base_uri,
                      const char   *rev,
                      GError      **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;
  gboolean did_exist;

  if (!store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_COMMIT, &did_exist, error))
    goto out;

  if (did_exist)
    log_verbose ("Already have commit %s", rev);
  else
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &commit, error))
        goto out;
      
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
      g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);
      
      if (!store_object (repo, soup, base_uri, tree_meta_checksum, OSTREE_OBJECT_TYPE_DIR_META, &did_exist, error))
        goto out;
      
      if (!store_tree_recurse (repo, soup, base_uri, tree_contents_checksum, error))
        goto out;
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  return ret;
}
                      
static gboolean
ostree_builtin_pull (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *remote;
  const char *branch;
  char *remote_branch_ref_path = NULL;
  char *key = NULL;
  char *baseurl = NULL;
  char *refpath = NULL;
  char *temppath = NULL;
  GFile *tempf = NULL;
  char *remote_ref = NULL;
  char *original_rev = NULL;
  GKeyFile *config = NULL;
  SoupURI *base_uri = NULL;
  SoupURI *target_uri = NULL;
  SoupSession *soup = NULL;
  char *rev = NULL;

  context = g_option_context_new ("REMOTE BRANCH - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "REMOTE and BRANCH must be specified", error);
      goto out;
    }

  remote = argv[1];
  branch = argv[2];

  remote_ref = g_strdup_printf ("%s/%s", remote, branch);

  if (!ostree_repo_resolve_rev (repo, remote_ref, TRUE, &original_rev, error))
    goto out;

  config = ostree_repo_get_config (repo);

  key = g_strdup_printf ("remote \"%s\"", remote);
  baseurl = g_key_file_get_string (config, key, "url", error);
  if (!baseurl)
    goto out;
  base_uri = soup_uri_new (baseurl);
  if (!base_uri)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse url '%s'", baseurl);
      goto out;
    }
  target_uri = soup_uri_copy (base_uri);
  g_free (refpath);
  refpath = g_build_filename (soup_uri_get_path (target_uri), "refs", "heads", branch, NULL);
  soup_uri_set_path (target_uri, refpath);
  
  soup = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_GNOME_FEATURES_2_26,
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_CONTENT_DECODER,
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_COOKIE_JAR,
                                             NULL);
  if (!fetch_uri (repo, soup, target_uri, &temppath, error))
    goto out;
  tempf = ot_gfile_new_for_path (temppath);

  if (!ot_gfile_load_contents_utf8 (tempf, &rev, NULL, NULL, error))
    goto out;
  g_strchomp (rev);

  if (original_rev && strcmp (rev, original_rev) == 0)
    {
      g_print ("No changes in %s\n", remote_ref);
    }
  else
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;

      if (!ostree_repo_prepare_transaction (repo, NULL, error))
        goto out;
      
      if (!store_commit_recurse (repo, soup, base_uri, rev, error))
        goto out;

      if (!ostree_repo_commit_transaction (repo, NULL, error))
        goto out;
      
      if (!ostree_repo_write_ref (repo, remote, branch, rev, error))
        goto out;
      
      g_print ("remote %s is now %s\n", remote_ref, rev);
    }
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (temppath)
    (void) unlink (temppath);
  g_free (temppath);
  g_free (key);
  g_free (rev);
  g_free (remote_ref);
  g_free (original_rev);
  g_free (baseurl);
  g_free (refpath);
  g_free (remote_branch_ref_path);
  g_clear_object (&soup);
  if (base_uri)
    soup_uri_free (base_uri);
  if (target_uri)
    soup_uri_free (target_uri);
  g_clear_object (&repo);
  g_clear_object (&soup);
  return ret;
}

static OstreeBuiltin builtins[] = {
  { "pull", ostree_builtin_pull, 0 },
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  return ostree_main (argc, argv, builtins);
}
