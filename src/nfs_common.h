#ifndef NFS_COMMON_H
#define NFS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "xdr.h"

/* Zero byte used for padding */
extern const uint8_t nfs_pad;

/*
 * nfsstat values from RFC 1094 (v2) and RFC 1813 (v3).
 * The numeric values are the same in both versions.
 */
#define NFS_OK		0
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
#define NFSERR_INVAL	22	/* v3 only */
#define NFSERR_FBIG	27
#define NFSERR_NOSPC	28
#define NFSERR_ROFS	30
#define NFSERR_MLINK	31	/* v3 only */
#define NFSERR_NAMETOOLONG	63
#define NFSERR_NOTEMPTY	66
#define NFSERR_DQUOT	69
#define NFSERR_STALE	70
#define NFSERR_BADHANDLE	10001	/* v3 only */
#define NFSERR_NOT_SYNC	10002	/* v3 only: SETATTR guard failed */
#define NFSERR_BAD_COOKIE	10003	/* v3 only */
#define NFSERR_NOTSUPP	10004	/* v3 only */
#define NFSERR_TOOSMALL	10005	/* v3 only */
#define NFSERR_SERVERFAULT	10006	/* v3 only */

/* NFSv2 file types */
#define NFNF	0
#define NFREG	1
#define NFDIR	2
#define NFBLK	3
#define NFCHR	4
#define NFLNK	5
#define NFSOCK	6
#define NFFIFO	7

/*
 * Decode one file name component from the request.
 * Return its length, or 0 if the name is empty, too long, or unsafe.
 * A safe name has no '/' (no NUL: xdr_get_string rejects it).
 * "." and ".." pass only if allow_dots is set (LOOKUP only).
 * "out" must hold maxlen + 1 bytes.
 * Do not use this for symlink targets: they may contain '/'.
 */
size_t nfs_get_name(struct xdr *x, char *out, size_t maxlen, int allow_dots);

/* Return 1 if "name" is a safe single path component. */
int nfs_name_ok(const char *name, int allow_dots);

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

/* Encode pre_op_attr (wcc_attr: size + mtime + ctime) */
void nfs3_enc_pre_op_attr(struct xdr *x, const struct fs_fattr *attr);

/* Encode post_op_attr from a pre-computed fattr (avoids redundant fhstat) */
void enc_postop_attr_fattr(struct xdr *x, const struct fs_fattr *attr);

#endif
