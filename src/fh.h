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
 * Return 1 if the export for this handle is read-only, or if no export
 * is usable for this client. Safe to call when the lookup fails.
 */
int  fh_export_ro(const struct nfs_fh *nfh);

/*
 * Path cache: handle -> path. See fh.c.
 * A returned path stays valid until the next cache change.
 */
#define FH_PATH_CACHE_SIZE 4096
void fh_path_cache_add(const struct nfs_fh *nfh, const char *path);
void fh_path_cache_add_bypath(const fhandle_t *fh, const char *path);
const char *fh_path_cache_get(const struct nfs_fh *nfh);
const char *fh_path_cache_getbyfh(const fhandle_t *fh);

/* After REMOVE/RMDIR: drop "path" and everything below it. */
void fh_path_cache_forget(const char *path);
/* After RENAME: rewrite cached paths from "from" to "to". */
void fh_path_cache_rename(const char *from, const char *to);

/*
 * Find the current path of a handle. Return 0 and fill "buf",
 * or -1 if the path is unknown. Never changes the working directory.
 *
 * fh_resolve_path:    for files and symlinks. Cache first, then (FreeBSD)
 *                     the kernel. Never use the kernel first: fhopen()
 *                     follows symlinks.
 * fh_resolve_dirpath: for directories. Kernel first (FreeBSD), then cache.
 */
int fh_resolve_path(const struct nfs_fh *nfh, char *buf, size_t sz);
int fh_resolve_dirpath(const struct nfs_fh *nfh, char *buf, size_t sz);

#endif
