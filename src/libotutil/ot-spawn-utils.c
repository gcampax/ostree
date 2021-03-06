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

#include "otutil.h"

#include <string.h>

gboolean
ot_spawn_sync_checked (const char           *cwd,
                       char                **argv,
                       char                **envp,
                       GSpawnFlags           flags,
                       GSpawnChildSetupFunc  child_setup,
                       gpointer              user_data,
                       char                **stdout_data,
                       char                **stderr_data,
                       GError              **error)
{
  gboolean ret = FALSE;
  gint exit_status;
  char *ret_stdout_data = NULL;
  char *ret_stderr_data = NULL;

  if (!g_spawn_sync (cwd, argv, envp, flags, child_setup, user_data,
                     stdout_data ? &ret_stdout_data : NULL,
                     stderr_data ? &ret_stderr_data : NULL,
                     &exit_status,
                     error))
    goto out;
  
  if (!g_spawn_check_exit_status (exit_status, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
