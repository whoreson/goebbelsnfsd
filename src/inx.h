#ifndef INX_H
#define INX_H

#include <stdint.h>
#include <sys/types.h>

/*
 * On-disk inode index.
 *
 * After a server restart, the in-memory path cache is empty.
 * Clients reuse stale file handles, so we need to resolve paths
 * from inode numbers. A brute-force walk is too slow (minutes),
 * so we pre-build an index and store it on disk.
 *
 * Format (all big-endian):
 *   Header:
 *     uint64_t magic   = 0x474E464844580001 ("GNFHDX\0\1")
 *     uint64_t timestamp (mtime of the export root at build time)
 *     uint32_t count
 *     uint8_t  padding[4]
 *   Entries (sorted by dev, then ino):
 *     uint64_t dev
 *     uint64_t ino
 *     uint32_t pathlen
 *     char     path[pathlen]
 */

#define INX_MAGIC  0x474E464844580001ULL
#define INX_VER    1
#define INX_PATH   "/var/cache/gnfsd/inx.bin"

int inx_build(const char *exportpath, const char *outfile);
const char *inx_lookup(const char *infile, uint64_t dev, uint64_t ino);
void inx_preload(const char *infile);

#endif /* INX_H */
