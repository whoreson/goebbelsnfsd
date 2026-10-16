#ifndef INX_H
#define INX_H

#include <stdint.h>
#include <sys/types.h>

/*
 * On-disk symlink index (version 2).
 *
 * After a restart the in-memory path cache is empty, but clients
 * still send old file handles. READLINK on a symlink handle cannot
 * use fhopen() (it fails with EMLINK on FreeBSD), so the server must
 * find the path from the inode number. A full directory walk takes
 * minutes. This index maps (dev, ino) -> path for every symlink below
 * the exports, and is searched with binary search.
 *
 * Only symlinks are stored. READLINK is the only user of this index.
 *
 * The file is a local cache. It uses the byte order of the host that
 * built it. A file from a host with different byte order fails the
 * magic check and is ignored.
 *
 * Layout (the file size must equal the sum of all parts):
 *   struct inx_hdr   (24 bytes)
 *   struct inx_rec   (24 bytes) * count, sorted by (dev, ino)
 *   string pool      (pool_len bytes); every string ends with NUL
 *
 * The index can be out of date. The caller MUST check the result
 * with lstat() (dev, ino, S_ISLNK) before it uses the path.
 */

#define INX_MAGIC  0x474E464844580002ULL	/* "GNFHDX\0\2" */
#define INX_PATH   "/var/cache/gnfsd/inx.bin"

struct inx_hdr {
	uint64_t magic;
	uint64_t built;		/* time(2) of build; informational only */
	uint32_t count;
	uint32_t pool_len;
};

struct inx_rec {
	uint64_t dev;
	uint64_t ino;
	uint32_t off;		/* offset in string pool */
	uint32_t len;		/* length including the NUL byte */
};

/*
 * Build an index of all symlinks below "roots" (nroots paths) and
 * write it to "outfile" atomically (temporary file + rename).
 * Returns 0 on success, -1 on error. The old file stays on error.
 */
int inx_build(const char *const *roots, unsigned nroots, const char *outfile);

/*
 * Load (or reload) the index. Returns the number of entries, or -1
 * if the file is missing or invalid (the old mapping is then kept).
 */
int inx_preload(const char *infile);

/*
 * Find the path for (dev, ino). Returns a NUL-terminated string that
 * stays valid until the next inx_preload(), or NULL if not found.
 */
const char *inx_lookup(uint64_t dev, uint64_t ino);

#endif /* INX_H */
