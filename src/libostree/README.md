Repository design
-----------------

At the heart of OSTree is the repository.  It's very similar to git,
with the idea of content-addressed storage.  However, OSTree is
designed to store operating system binaries, not source code.  There
are several consequences to this.  The key difference as compared to
git is that the OSTree definition of "content" includes key Unix
metadata such as owner uid/gid, as well as all extended attributes.

Essentially OSTree is designed so that if two files have the same
OSTree checksum, it's safe to replace them with a hard link.  This
fundamental design means that an OSTree repository imposes negligible
overhead.  In contrast, a git repository stores copies of
zlib-compressed data.

Key differences versus git
----------------------------

 * As mentioned above, extended attributes and owner uid/gid are versioned
 * SHA256 instead of SHA1
 * Support for empty directories

Binary files
------------

While this is still in planning, I plan to heavily optimize OSTree for
versioning ELF operating systems.  In industry jargon, this would be
"content-aware storage".

