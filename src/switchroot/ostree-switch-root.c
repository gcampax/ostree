/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 * Copyright 2011,2012 Colin Walters <walters@verbum.org>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>

static int
perrorv (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static int
perrorv (const char *format, ...)
{
  va_list args;
  char buf[PATH_MAX];
  char *p;

  p = strerror_r (errno, buf, sizeof (buf));

  va_start (args, format);

  vfprintf (stderr, format, args);
  fprintf (stderr, ": %s\n", p);
  fflush (stderr);

  va_end (args);

  sleep (3);
	
  return 0;
}

int
main(int argc, char *argv[])
{
  const char *initramfs_move_mounts[] = { "/dev", "/proc", "/sys", "/run", NULL };
  const char *toproot_bind_mounts[] = { "/home", "/root", "/tmp", NULL };
  const char *ostree_bind_mounts[] = { "/var", NULL };
  const char *readonly_bind_mounts[] = { "/bin", "/etc", "/lib", "/sbin", "/usr",
					 NULL };
  const char *ostree_root = NULL;
  const char *ostree_subinit = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  char **init_argv = NULL;
  int i;
  int before_init_argc = 0;

  if (argc < 3)
    {
      fprintf (stderr, "usage: ostree-switch-root NEWROOT INIT [ARGS...]\n");
      exit (1);
    }

  before_init_argc++;
  ostree_root = argv[1];
  before_init_argc++;
  ostree_subinit = argv[2];
  before_init_argc++;

  snprintf (destpath, sizeof(destpath), "/ostree/%s", ostree_root);
  if (stat (destpath, &stbuf) < 0)
    {
      perrorv ("Invalid ostree root '%s'", destpath);
      exit (1);
    }
  
  snprintf (destpath, sizeof(destpath), "/ostree/%s/var", ostree_root);
  if (mount ("/ostree/var", destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount / to '%s'", destpath);
      exit (1);
    }
  
  snprintf (destpath, sizeof(destpath), "/ostree/%s/sysroot", ostree_root);
  if (mount ("/", destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount / to '%s'", destpath);
      exit (1);
    }

  for (i = 0; initramfs_move_mounts[i] != NULL; i++)
    {
      const char *path = initramfs_move_mounts[i];
      snprintf (srcpath, sizeof(srcpath), path);
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, path);
      if (mount (srcpath, destpath, NULL, MS_MOVE, NULL) < 0)
	{
	  perrorv ("failed to move mount of %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  for (i = 0; toproot_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, toproot_bind_mounts[i]);
      if (mount (toproot_bind_mounts[i], destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:toproot) %s to %s", toproot_bind_mounts[i], destpath);
	  exit (1);
	}
    }

  for (i = 0; ostree_bind_mounts[i] != NULL; i++)
    {
      snprintf (srcpath, sizeof(srcpath), "/ostree/%s", ostree_bind_mounts[i]);
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, ostree_bind_mounts[i]);
      if (mount (srcpath, destpath, NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:bind) %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  for (i = 0; readonly_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, readonly_bind_mounts[i]);
      if (mount (destpath, destpath, NULL, MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (1);
	}
      if (mount (destpath, destpath, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (1);
	}
    }
  
  snprintf (destpath, sizeof(destpath), "/ostree/%s", ostree_root);
  if (chroot (destpath) < 0)
    {
      perrorv ("failed to change root to '%s'", destpath);
      exit (1);
    }

  if (chdir ("/") < 0)
    {
      perrorv ("failed to chdir to subroot");
      exit (1);
    }

  init_argv = malloc (sizeof (char*)*((argc-before_init_argc)+2));
  init_argv[0] = (char*)ostree_subinit;
  for (i = 0; i < argc-before_init_argc; i++)
    init_argv[i+1] = argv[i+before_init_argc];
  init_argv[i+1] = NULL;
  
  fprintf (stderr, "ostree-init: Running real init %s (argc=%d)\n", init_argv[0], argc-before_init_argc);
  fflush (stderr);
  execv (init_argv[0], init_argv);
  perrorv ("Failed to exec init '%s'", init_argv[0]);
  exit (1);
}
