#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>

#include "nfs_common.h"
#include "types.h"

/* Zero byte used for padding */
const uint8_t nfs_pad = 0;

uint32_t
nfs_mode_to_type(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return NFREG;
	case S_IFDIR:	return NFDIR;
	case S_IFBLK:	return NFBLK;
	case S_IFCHR:	return NFCHR;
	case S_IFLNK:	return NFLNK;
	case S_IFSOCK:	return NFSOCK;
	case S_IFIFO:	return NFFIFO;
	default:	 return NFNF;
	}
}

uint32_t
nfs_errno(uint32_t e)
{
	switch (e) {
	case 0:	 return NFS_OK;
	case EPERM:	 return NFSERR_PERM;
	case ENOENT:	 return NFSERR_NOENT;
	case EIO:	 return NFSERR_IO;
	case ENXIO:	 return NFSERR_NXIO;
	case EACCES:	 return NFSERR_ACCES;
	case EEXIST:	 return NFSERR_EXIST;
	case EXDEV:	 return NFSERR_XDEV;
	case ENODEV:	 return NFSERR_NODEV;
	case ENOTDIR:	 return NFSERR_NOTDIR;
	case EISDIR:	 return NFSERR_ISDIR;
	case EINVAL:	 return NFSERR_INVAL;
	case ENAMETOOLONG: return NFSERR_NAMETOOLONG;
	default:	 return NFSERR_PERM;
	}
}

void
nfs2_enc_fattr(struct xdr *x, const struct fs_fattr *attr)
{
	/* NFSv2 fattr (68 bytes):
	 * fa_type, fa_mode, fa_nlink, fa_uid, fa_gid,
	 * fa_size, fa_blocksize, fa_rdev, fa_blocks,
	 * fa_fsid, fa_fileid,
	 * fa_atime(sec+usec), fa_mtime(sec+usec), fa_ctime(sec+usec) */

	/* fa_type (from mode) */
	xdr_put_u32(x, nfs_mode_to_type(attr->mode));
	/* fa_mode */
	xdr_put_u32(x, attr->mode);
	/* fa_nlink */
	xdr_put_u32(x, (uint32_t)attr->nlink);
	/* fa_uid */
	xdr_put_u32(x, attr->uid);
	/* fa_gid */
	xdr_put_u32(x, attr->gid);
	/* fa_size */
	xdr_put_u32(x, (uint32_t)attr->size);
	/* fa_blocksize */
	xdr_put_u32(x, 1024);
	/* fa_rdev */
	xdr_put_u32(x, attr->rdev_spec[0] * 256 + attr->rdev_spec[1]);
	/* fa_blocks */
	xdr_put_u32(x, (uint32_t)attr->used);
	/* fa_fsid */
	xdr_put_u32(x, (uint32_t)(attr->fsid & 0xFFFFFFFF));
	/* fa_fileid */
	xdr_put_u32(x, (uint32_t)(attr->fileid & 0xFFFFFFFF));
	/* fa_atime */
	xdr_put_u32(x, (uint32_t)attr->atime_sec);
	xdr_put_u32(x, (uint32_t)attr->atime_usec);
	/* fa_mtime */
	xdr_put_u32(x, (uint32_t)attr->mtime_sec);
	xdr_put_u32(x, (uint32_t)attr->mtime_usec);
	/* fa_ctime */
	xdr_put_u32(x, (uint32_t)attr->ctime_sec);
	xdr_put_u32(x, (uint32_t)attr->ctime_usec);
}

void
nfs3_enc_fattr(struct xdr *x, const struct fs_fattr *attr)
{
	/* fa_type (4 bytes) */
	xdr_put_u32(x, nfs_mode_to_type(attr->mode));
	/* fa_mode (4 bytes) */
	xdr_put_u32(x, attr->mode);
	/* fa_nlink (4 bytes, NOT u64) */
	xdr_put_u32(x, (uint32_t)attr->nlink);
	/* fa_uid (4 bytes) */
	xdr_put_u32(x, attr->uid);
	/* fa_gid (4 bytes) */
	xdr_put_u32(x, attr->gid);
	/* fa_size (8 bytes) */
	xdr_put_u64(x, attr->size);
	/* fa_used (8 bytes) */
	xdr_put_u64(x, attr->used);
	/* fa_rdev (8 bytes: specdata1 + specdata2) */
	xdr_put_u32(x, attr->rdev_spec[0]);
	xdr_put_u32(x, attr->rdev_spec[1]);
	/* fa_fsid (8 bytes) */
	xdr_put_u64(x, attr->fileid);
	/* fa_fileid (8 bytes) */
	xdr_put_u64(x, attr->fileid);
	/* fa_atime (8 bytes: seconds + nseconds, both uint32) */
	xdr_put_u32(x, (uint32_t)attr->atime_sec);
	xdr_put_u32(x, (uint32_t)attr->atime_usec * 1000);
	/* fa_mtime (8 bytes) */
	xdr_put_u32(x, (uint32_t)attr->mtime_sec);
	xdr_put_u32(x, (uint32_t)attr->mtime_usec * 1000);
	/* fa_ctime (8 bytes) */
	xdr_put_u32(x, (uint32_t)attr->ctime_sec);
	xdr_put_u32(x, (uint32_t)attr->ctime_usec * 1000);
	/* Total: 4*5 + 8*8 = 84 bytes = NFSX_V3FATTR */
}

void
nfs3_enc_wcc_attr(struct xdr *x, const struct fs_fattr *attr)
{
	xdr_put_u64(x, attr->size);
	xdr_put_u64(x, (uint64_t)attr->mtime_sec);
	xdr_put_u32(x, (uint32_t)attr->mtime_usec * 1000);
	xdr_put_u64(x, (uint64_t)attr->ctime_sec);
	xdr_put_u32(x, (uint32_t)attr->ctime_usec * 1000);
}

uint64_t
nfs3_changeattr(const struct fs_fattr *attr)
{
	return (uint64_t)attr->ctime_sec * 1000000 +
	    (uint64_t)attr->ctime_usec;
}

void
nfs2_enc_statfs(struct xdr *x, const struct statfs *sf)
{
	xdr_put_u32(x, sf->f_bsize);	/* tsize */
	xdr_put_u32(x, sf->f_bsize);	/* bsize */
	xdr_put_u32(x, sf->f_blocks);	/* blocks */
	xdr_put_u32(x, sf->f_bfree);	/* bfree */
	xdr_put_u32(x, sf->f_bavail);	/* bavail */
}

void
nfs3_enc_fsstat(struct xdr *x, const struct statfs *sf)
{
	xdr_put_u64(x, sf->f_blocks * (uint64_t)sf->f_bsize);	/* tbytes */
	xdr_put_u64(x, sf->f_bfree * (uint64_t)sf->f_bsize);	/* bytes */
	xdr_put_u64(x, sf->f_bavail * (uint64_t)sf->f_bsize);	/* tbytes avail */
	xdr_put_u64(x, sf->f_bavail * (uint64_t)sf->f_bsize);	/* bytes avail */
	xdr_put_u32(x, NFS3_MAXDATA_TCP);	/* rtransfersize */
	xdr_put_u32(x, NFS3_MAXDATA_TCP);	/* wtransfersize */
	xdr_put_u32(x, NFS3_MAXDATA_TCP);	/* prefered transfersize */
	xdr_put_u64(x, sf->f_bsize);	/* maxtransfersize */
	xdr_put_u64(x, 0);	/* max links (unknown) */
}
