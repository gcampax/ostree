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

#ifndef _OSTREE_CORE
#define _OSTREE_CORE

#include <otutil.h>

G_BEGIN_DECLS

#define OSTREE_MAX_METADATA_SIZE (1 << 26)

#define OSTREE_EMPTY_STRING_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

typedef enum {
  OSTREE_OBJECT_TYPE_RAW_FILE = 1,   /* .raw */
  OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT = 2,  /* .archive-content */
  OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META = 3,  /* .archive-meta */
  OSTREE_OBJECT_TYPE_DIR_TREE = 4,  /* .dirtree */
  OSTREE_OBJECT_TYPE_DIR_META = 5,  /* .dirmeta */
  OSTREE_OBJECT_TYPE_COMMIT = 6     /* .commit */
} OstreeObjectType;

#define OSTREE_OBJECT_TYPE_IS_META(t) (t >= 3 && t <= 6)
#define OSTREE_OBJECT_TYPE_LAST OSTREE_OBJECT_TYPE_COMMIT

#define OSTREE_SERIALIZED_VARIANT_FORMAT G_VARIANT_TYPE("(uv)")

/*
 * xattr objects:
 * a(ayay) - array of (name, value) pairs, both binary data, though name is a bytestring
 */
#define OSTREE_XATTR_GVARIANT_FORMAT "a(ayay)"

#define OSTREE_DIR_META_VERSION 0
/*
 * dirmeta objects:
 * u - Version
 * u - uid
 * u - gid
 * u - mode
 * a(ayay) - xattrs
 */
#define OSTREE_DIRMETA_GVARIANT_FORMAT "(uuuua(ayay))"

#define OSTREE_TREE_VERSION 0
/*
 * Tree objects:
 * u - Version
 * a{sv} - Metadata
 * a(ss) - array of (filename, checksum) for files
 * a(sss) - array of (dirname, tree_checksum, meta_checksum) for directories
 */
#define OSTREE_TREE_GVARIANT_FORMAT "(ua{sv}a(ss)a(sss)"

#define OSTREE_COMMIT_VERSION 0
/*
 * Commit objects:
 * u - Version
 * a{sv} - Metadata
 * s - parent checksum (empty string for initial)
 * s - subject 
 * s - body
 * t - Timestamp in seconds since the epoch (UTC)
 * s - Root tree contents
 * s - Root tree metadata
 */
#define OSTREE_COMMIT_GVARIANT_FORMAT "(ua{sv}ssstss)"

/* Archive file objects:
 * u - Version
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target
 * a(ayay) - xattrs
 */
#define OSTREE_ARCHIVED_FILE_VARIANT_FORMAT G_VARIANT_TYPE ("(uuuuusa(ayay))")

/* Pack super index
 * s - OSTSUPERPACKINDEX
 * a{sv} - Metadata
 * a(say) - (pack file checksum, bloom filter)
 */
#define OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT G_VARIANT_TYPE ("(sa{sv}a(say))")

/* Pack index
 * s - OSTv0PACKINDEX
 * a{sv} - Metadata
 * a(uayt) - (objtype, checksum, offset into packfile)
 */
#define OSTREE_PACK_INDEX_VARIANT_FORMAT G_VARIANT_TYPE ("(sa{sv}a(uayt))")

typedef enum {
  OSTREE_PACK_FILE_ENTRY_FLAG_NONE = 0,
  OSTREE_PACK_FILE_ENTRY_FLAG_GZIP = (1 << 0)
} OstreePackFileEntryFlag;

/* Pack files
 * s - OSTv0PACKFILE
 * a{sv} - Metadata
 * t - number of entries
 *
 * Repeating pair of:
 * <padding to alignment of 8>
 * ( uyayay ) - objtype, flags, checksum, data
 */
#define OSTREE_PACK_FILE_VARIANT_FORMAT G_VARIANT_TYPE ("(sa{sv}t)")

#define OSTREE_PACK_FILE_CONTENT_VARIANT_FORMAT G_VARIANT_TYPE ("(uyayay)")

gboolean ostree_validate_checksum_string (const char *sha256,
                                          GError    **error);

GVariant *ostree_checksum_to_bytes (const char *sha256);

gboolean ostree_validate_rev (const char *rev, GError **error);

void ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode);

const char * ostree_object_type_to_string (OstreeObjectType objtype);

OstreeObjectType ostree_object_type_from_string (const char *str);

guint ostree_hash_object_name (gconstpointer a);

int ostree_cmp_checksum_bytes (GVariant *a, GVariant *b);

GVariant *ostree_object_name_serialize (const char *checksum,
                                        OstreeObjectType objtype);

void ostree_object_name_deserialize (GVariant         *variant,
                                     const char      **out_checksum,
                                     OstreeObjectType *out_objtype);

GVariant *ostree_object_name_serialize_v2 (const char *checksum,
                                           OstreeObjectType objtype);

void ostree_object_name_deserialize_v2_hex (GVariant         *variant,
                                            char            **out_checksum,
                                            OstreeObjectType *out_objtype);

void ostree_object_name_deserialize_v2_bytes (GVariant         *variant,
                                              const guchar    **out_checksum,
                                              OstreeObjectType *out_objtype);

GVariant * ostree_checksum_to_bytes (const char *sha256);
char * ostree_checksum_from_bytes (GVariant *bytes);

char * ostree_object_to_string (const char *checksum,
                                OstreeObjectType objtype);

void ostree_object_from_string (const char *str,
                                gchar     **out_checksum,
                                OstreeObjectType *out_objtype);

char *ostree_get_relative_object_path (const char        *checksum,
                                       OstreeObjectType   type);

GVariant *ostree_get_xattrs_for_file (GFile       *f,
                                      GError     **error);

GVariant *ostree_wrap_metadata_variant (OstreeObjectType type, GVariant *metadata);

gboolean ostree_set_xattrs (GFile *f, GVariant *xattrs,
                            GCancellable *cancellable, GError **error);

gboolean ostree_map_metadata_file (GFile                       *file,
                                   OstreeObjectType             expected_type,
                                   GVariant                   **out_variant,
                                   GError                     **error);

gboolean ostree_checksum_file_from_input (GFileInfo        *file_info,
                                          GVariant         *xattrs,
                                          GInputStream     *in,
                                          OstreeObjectType  objtype,
                                          GChecksum       **out_checksum,
                                          GCancellable     *cancellable,
                                          GError          **error);

gboolean ostree_checksum_file (GFile             *f,
                               OstreeObjectType   type,
                               GChecksum        **out_checksum,
                               GCancellable      *cancellable,
                               GError           **error);

void ostree_checksum_file_async (GFile                 *f,
                                 OstreeObjectType       objtype,
                                 int                    io_priority,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data);

gboolean ostree_checksum_file_async_finish (GFile          *f,
                                            GAsyncResult   *result,
                                            GChecksum     **out_checksum,
                                            GError        **error);

GVariant *ostree_create_directory_metadata (GFileInfo *dir_info,
                                            GVariant  *xattrs);

gboolean ostree_create_file_from_input (GFile          *file,
                                        GFileInfo      *finfo,
                                        GVariant       *xattrs,
                                        GInputStream   *input,
                                        OstreeObjectType objtype,
                                        GChecksum     **out_checksum,
                                        GCancellable   *cancellable,
                                        GError        **error);

gboolean ostree_create_temp_file_from_input (GFile            *dir,
                                             const char       *prefix,
                                             const char       *suffix,
                                             GFileInfo        *finfo,
                                             GVariant         *xattrs,
                                             GInputStream     *input,
                                             OstreeObjectType objtype,
                                             GFile           **out_file,
                                             GChecksum       **out_checksum,
                                             GCancellable     *cancellable,
                                             GError          **error);

gboolean ostree_create_temp_regular_file (GFile            *dir,
                                          const char       *prefix,
                                          const char       *suffix,
                                          GFile           **out_file,
                                          GOutputStream   **out_stream,
                                          GCancellable     *cancellable,
                                          GError          **error);

gboolean ostree_create_temp_hardlink (GFile            *dir,
                                      GFile            *src,
                                      const char       *prefix,
                                      const char       *suffix,
                                      GFile           **out_file,
                                      GCancellable     *cancellable,
                                      GError          **error);

GVariant *ostree_create_archive_file_metadata (GFileInfo   *file_info,
                                               GVariant    *xattrs);

gboolean ostree_parse_archived_file_meta (GVariant         *data,
                                          GFileInfo       **out_file_info,
                                          GVariant        **out_xattrs,
                                          GError          **error);

gboolean ostree_read_pack_entry_raw (guchar           *pack_data,
                                     guint64           pack_len,
                                     guint64           object_offset,
                                     gboolean          trusted,
                                     GVariant        **out_entry,
                                     GCancellable     *cancellable,
                                     GError          **error);

GInputStream *ostree_read_pack_entry_as_stream (GVariant *pack_entry);

gboolean ostree_read_pack_entry_variant (GVariant         *pack_entry,
                                         OstreeObjectType  expected_objtype,
                                         gboolean          trusted,
                                         GVariant        **out_variant,
                                         GCancellable     *cancellable,
                                         GError          **error);

#endif /* _OSTREE_REPO */
