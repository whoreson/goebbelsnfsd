#ifndef FH_H
#define FH_H

#include <sys/mount.h>
#include <stdint.h>

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

#endif
