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

#ifndef _HACKTREE_CORE
#define _HACKTREE_CORE

#include <htutil.h>

G_BEGIN_DECLS

#define HACKTREE_EMPTY_STRING_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

typedef enum {
  HACKTREE_SERIALIZED_TREE_VARIANT = 1,
  HACKTREE_SERIALIZED_COMMIT_VARIANT = 2
  HACKTREE_SERIALIZED_XATTRS_VARIANT = 3
} HacktreeSerializedVariantType;

#define HACKTREE_SERIALIZED_VARIANT_FORMAT G_VARIANT_TYPE("(uv)")

#define HACKTREE_TREE_VERSION 0
/*
 * Tree objects:
 * u - Version
 * a{sv} - Metadata
 * a(ss) - array of (checksum, filename) for files
 * as - array of tree checksums for directories
 * a(suuus) - array of (dirname, uid, gid, mode, xattr_checksum) for directories
 */
#define HACKTREE_TREE_GVARIANT_FORMAT G_VARIANT_TYPE("(ua{sv}a(ss)asa(suuus)")

#define HACKTREE_COMMIT_VERSION 0
/*
 * Commit objects:
 * u - Version
 * a{sv} - Metadata
 * s - subject 
 * s - body
 * t - Timestamp in seconds since the epoch (UTC)
 * s - Tree SHA256
 */
#define HACKTREE_COMMIT_GVARIANT_FORMAT G_VARIANT_TYPE("(ua{sv}ssts)")

/*
 * xattr objects:
 * u - Version
 * ay - data
 */
#define HACKTREE_XATTR_GVARIANT_FORMAT G_VARIANT_TYPE("(uay)")

gboolean   hacktree_get_xattrs_for_directory (const char *path,
                                              char      **out_xattrs,
                                              gsize      *out_len,
                                              GError    **error);

gboolean hacktree_stat_and_checksum_file (int dirfd, const char *path,
                                          GChecksum **out_checksum,
                                          struct stat *out_stbuf,
                                          GError **error);


#endif /* _HACKTREE_REPO */
