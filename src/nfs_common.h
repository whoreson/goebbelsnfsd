#ifndef NFS_COMMON_H
#define NFS_COMMON_H

#include <stdint.h>

#include "fs.h"
#include "xdr.h"

/* Zero byte used for padding */
extern const uint8_t nfs_pad;

/* NFSv2/nfsstat values */
#define NFS_OK	0
#define NFSERR_PERM	1
#define NFSERR_NOENT	2
#define NFSERR_IO	5
#define NFSERR_NXIO	6
#define NFSERR_ACCES	13
#define NFSERR_EXIST	17
#define NFSERR_XDEV	18
#define NFSERR_NODEV	19
#define NFSERR_NOTDIR	20
#define NFSERR_ISDIR	21
#define NFSERR_INVAL	29
#define NFSERR_NAMETOOLONG	63
#define NFSERR_STALE	70
#define NFSERR_ROFS	30
#define NFSERR_NOTSUPP	10004

/* NFSv2 file types */
#define NFNF	0
#define NFREG	1
#define NFDIR	2
#define NFBLK	3
#define NFCHR	4
#define NFLNK	5
#define NFSOCK	6
#define NFFIFO	7

/* Convert POSIX mode to NFSv2 file type */
uint32_t nfs_mode_to_type(uint32_t mode);

/* Convert POSIX errno to nfsstat */
uint32_t nfs_errno(uint32_t e);

/* Encode fattr into XDR output for v2 (3 attributes: mode, uid/gid, size) */
void nfs2_enc_fattr(struct xdr *x, const struct fs_fattr *attr);

/* Encode fattr into XDR output for v3 (full attribute list) */
void nfs3_enc_fattr(struct xdr *x, const struct fs_fattr *attr);

/* NFSv3 post-op attributes */
void nfs3_enc_wcc_attr(struct xdr *x, const struct fs_fattr *attr);

/* NFSv3 change attribute (ctime) */
uint64_t nfs3_changeattr(const struct fs_fattr *attr);

/* Encode fsstat for v2 */
void nfs2_enc_statfs(struct xdr *x, const struct statfs *sf);

/* Encode fsstat for v3 */
void nfs3_enc_fsstat(struct xdr *x, const struct statfs *sf);

#endif
