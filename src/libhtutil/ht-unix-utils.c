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

#include "ht-unix-utils.h"

#include <glib-unix.h>
#include <gio/gio.h>

#include <string.h>
#include <sys/types.h>
#include <dirent.h>

static int
compare_filenames_by_component_length (const char *a,
                                       const char *b)
{
  char *a_slash, *b_slash;

  a_slash = strchr (a, '/');
  b_slash = strchr (b, '/');
  while (a_slash && b_slash)
    {
      a = a_slash + 1;
      b = b_slash + 1;
      a_slash = strchr (a, '/');
      b_slash = strchr (b, '/');
    }
  if (a_slash)
    return -1;
  else if (b_slash)
    return 1;
  else
    return 0;
}

GPtrArray *
ht_util_sort_filenames_by_component_length (GPtrArray *files)
{
  GPtrArray *array = g_ptr_array_sized_new (files->len);
  memcpy (array->pdata, files->pdata, sizeof (gpointer) * files->len);
  g_ptr_array_sort (array, (GCompareFunc) compare_filenames_by_component_length);
  return array;
}

int
ht_util_count_filename_components (const char *path)
{
  int i = 0;
  char *p;

  while (path)
    {
      i++;
      path = strchr (path, '/');
      if (path)
        path++;
    }
  return i;
}

gboolean
ht_util_filename_has_dotdot (const char *path)
{
  char *p;
  char last;

  if (strcmp (path, "..") == 0)
    return TRUE;
  if (g_str_has_prefix (path, "../"))
    return TRUE;
  p = strstr (path, "/..");
  last = *(p + 1);
  return last == '\0' || last == '/';
}

void
ht_util_set_error_from_errno (GError **error,
                              gint     saved_errno)
{
  g_set_error_literal (error,
                       G_UNIX_ERROR,
                       0,
                       g_strerror (saved_errno));
  errno = saved_errno;
}

int
ht_util_open_file_read (const char *path, GError **error)
{
  char *dirname = NULL;
  char *basename = NULL;
  DIR *dir = NULL;
  int fd = -1;

  dirname = g_path_get_dirname (path);
  basename = g_path_get_basename (path);
  dir = opendir (dirname);
  if (dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  fd = ht_util_open_file_read_at (dirfd (dir), basename, error);

 out:
  g_free (basename);
  g_free (dirname);
  if (dir != NULL)
    closedir (dir);
  return fd;
}

int
ht_util_open_file_read_at (int dirfd, const char *name, GError **error)
{
  int fd;
  int flags = O_RDONLY;
  
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOATIME
  flags |= O_NOATIME;
#endif
  fd = openat (dirfd, name, flags);
  if (fd < 0)
    ht_util_set_error_from_errno (error, errno);
  return fd;
}
