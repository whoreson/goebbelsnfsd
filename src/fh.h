#ifndef FH_H
#define FH_H

#include <stdint.h>
#include "port.h"

#define NFS_FH_MAGIC	0x55fdu
#define NFS_FH_SIZE	32

/*
 * Our internal file handle, stored as raw bytes for NFSv2 wire format.
 * 32 bytes: magic(2) + fid_len(2) + fsid(8) + fid(20)
 */
struct nfs_fh {
	uint8_t data[NFS_FH_SIZE];
};

/* Encode a kernel fhandle_t into our nfs_fh. Returns 0 on success. */
int  fh_encode(struct nfs_fh *nfh, const fhandle_t *fh);

/* Validate an nfs_fh (magic, size). Returns 1 if valid. */
int  fh_valid(const struct nfs_fh *nfh);

/* Decode an nfs_fh back into a kernel fhandle_t. Returns 0 on success. */
int  fh_decode(const struct nfs_fh *nfh, fhandle_t *fh);

/* Look up the export for this handle's fsid. Returns NULL if none. */
const struct export *fh_lookup_export(const struct nfs_fh *nfh);

/*
 * Path cache for symlinks: store the path when creating a file handle.
 * This is needed because fhopen follows symlinks, making it impossible
 * to read the symlink target from the file handle alone.
 */
#define FH_PATH_CACHE_SIZE 64
void fh_path_cache_add(const struct nfs_fh *nfh, const char *path);
const char *fh_path_cache_get(const struct nfs_fh *nfh);

#endif
