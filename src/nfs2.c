#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

#include "conf.h"
#include "dircache.h"
#include "dirlist.h"
#include "port.h"
#include "fh.h"
#include "fs.h"
#include "log.h"
#include "nfs_common.h"
#include "progs.h"
#include "rpc.h"
#include "types.h"

#define NFS2_MAXPATHLEN	255
#define NFS2_MAXRDIR	1024

static int nfs2_null(struct req *r);
static int nfs2_getattr(struct req *r);
static int nfs2_setattr(struct req *r);
static int nfs2_root(struct req *r);
static int nfs2_lookup(struct req *r);
static int nfs2_readlink(struct req *r);
static int nfs2_read(struct req *r);
static int nfs2_writecache(struct req *r);
static int nfs2_write(struct req *r);
static int nfs2_create(struct req *r);
static int nfs2_remove(struct req *r);
static int nfs2_rename(struct req *r);
static int nfs2_link(struct req *r);
static int nfs2_symlink(struct req *r);
static int nfs2_mkdir(struct req *r);
static int nfs2_rmdir(struct req *r);
static int nfs2_readdir(struct req *r);
static int nfs2_statfs(struct req *r);

static const struct rpc_proc nfs2_procs[] = {
	{ "NULL",       nfs2_null,    0 },	/*  0 */
	{ "GETATTR",    nfs2_getattr, 0 },	/*  1 */
	{ "SETATTR",    nfs2_setattr, 1 },	/*  2 */
	{ "ROOT",       nfs2_root,    0 },	/*  3 obsolete */
	{ "LOOKUP",     nfs2_lookup,  0 },	/*  4 */
	{ "READLINK",   nfs2_readlink,0 },	/*  5 */
	{ "READ",       nfs2_read,    0 },	/*  6 */
	{ "WRITECACHE", nfs2_writecache, 0 },	/*  7 obsolete */
	{ "WRITE",      nfs2_write,   1 },	/*  8 */
	{ "CREATE",     nfs2_create,  1 },	/*  9 */
	{ "REMOVE",     nfs2_remove,  1 },	/* 10 */
	{ "RENAME",     nfs2_rename,  1 },	/* 11 */
	{ "LINK",       nfs2_link,    1 },	/* 12 */
	{ "SYMLINK",    nfs2_symlink, 1 },	/* 13 */
	{ "MKDIR",      nfs2_mkdir,   1 },	/* 14 */
	{ "RMDIR",      nfs2_rmdir,   1 },	/* 15 */
	{ "READDIR",    nfs2_readdir, 0 },	/* 16 */
	{ "STATFS",     nfs2_statfs,  0 }	/* 17 */
};

const struct rpc_prog nfs2_prog = {
	RPC_PROG_NFS, 2, sizeof(nfs2_procs) / sizeof(nfs2_procs[0]), nfs2_procs
};

/*
 * Encode a file handle into the output XDR buffer (NFSv2: fixed 32 bytes).
 */
static void
enc_fh(struct xdr *x, const struct nfs_fh *nfh)
{
	xdr_put_fixed(x, nfh, NFS_FH_SIZE);
}

/*
 * Decode a file handle from the input XDR buffer (NFSv2: fixed 32 bytes).
 */
static int
dec_fh(struct xdr *x, struct nfs_fh *nfh)
{
	xdr_get_fixed(x, nfh, NFS_FH_SIZE);
	if (!xdr_ok(x))
	return -1;
	if (!fh_valid(nfh))
	return -1;
	return 0;
}

/*
 * Getattr helper: decode fh, do fhstat, encode reply.
 */
static uint32_t
do_getattr(struct req *r, struct nfs_fh *nfh, struct fs_fattr *attr)
{
	fhandle_t fh;
	uint32_t nstat;
	const struct export *ex;

	(void)r;
	if (fh_decode(nfh, &fh) < 0) {
	log_msg(L_DEBUG, "do_getattr: fh_decode failed");
	return NFSERR_STALE;
	}
	ex = fh_lookup_export(nfh);
	if (ex == NULL) {
	log_msg(L_DEBUG, "do_getattr: no export for fh");
	return NFSERR_ACCES;
	}
	nstat = fs_getattr(&fh, attr);
	if (nstat != 0) {
	log_msg(L_DEBUG, "do_getattr: fs_getattr failed: %d", nstat);
	return nfs_errno(nstat);
	}
	return NFS_OK;
}

static int
nfs2_null(struct req *r)
{
	(void)r;
	return PROC_OK;
}

static int
nfs2_getattr(struct req *r)
{
	struct nfs_fh nfh;
	struct fs_fattr attr;
	uint32_t nstat;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	nstat = do_getattr(r, &nfh, &attr);
	/* NFSv2 GETATTR reply: nfsstat status; union switch(status) {
	 * case NFS_OK: fattr; default: void; } */
	xdr_put_u32(&r->out, nstat);
	if (nstat == NFS_OK)
	nfs2_enc_fattr(&r->out, &attr);
	return PROC_OK;
}

static int
nfs2_root(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_lookup(struct req *r)
{
	struct nfs_fh dir_fh;
	struct fs_fattr attr;
	fhandle_t fh, dir_kfh;
	const struct export *ex;
	char name[NFS2_MAXPATHLEN + 1];
	char fullpath[MAX_PATH_LEN];
	char dirpath[MAX_PATH_LEN];
	size_t nlen;
	uint32_t nstat;

	if (dec_fh(&r->in, &dir_fh) < 0)
	return PROC_GARBAGE;
	ex = fh_lookup_export(&dir_fh);
	if (ex == NULL) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	nlen = nfs_get_name(&r->in, name, sizeof(name) - 1, 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (nlen == 0 || nlen > NFS2_MAXPATHLEN) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	return PROC_OK;
	}
	/* Resolve directory path from file handle, then append name */
	{
	if (fh_decode(&dir_fh, &dir_kfh) < 0 ||
	    fh_resolve_dirpath(&dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);
	}

	/* Check parent dir is accessible (dir_kfh already decoded above) */
	nstat = fs_getattr(&dir_kfh, &attr);
	if (nstat != NFS_OK) {
xdr_put_u32(&r->out, nstat);
	return PROC_OK;
	}
	/* Check execute permission on directory */
	if (fs_access(&attr, r->uid, r->gid, 0100) < 0) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}

	/* Handle . and .. specially */
	if (name[0] == '.' && name[1] == '\0') {
	memcpy(&fh, &dir_kfh, sizeof(fh));
	} else if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
	/* Stay at the export root: never leave the export. */
	if (dirpath[1] == '\0' || strcmp(dirpath, ex->path) == 0) {
	memcpy(&fh, &dir_kfh, sizeof(fh));
	} else {
	char *slash = strrchr(dirpath, '/');
	if (slash == dirpath) {
	if (port_lgetfh("/", &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	} else {
	*slash = '\0';
	if (port_lgetfh(dirpath, &fh) < 0) {
	*slash = '/';
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	*slash = '/';
	}
	}
	} else {
	log_msg(L_DEBUG, "LOOKUP: fullpath=%s", fullpath);
	if (port_lgetfh(fullpath, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	}

	/* Get attributes - use lstat to not follow symlinks */
	{
	struct stat sb;
	if (lstat(fullpath, &sb) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	memset(&attr, 0, sizeof(attr));
	attr.mode = sb.st_mode;
	attr.nlink = sb.st_nlink;
	attr.uid = sb.st_uid;
	attr.gid = sb.st_gid;
	attr.size = sb.st_size;
	attr.used = sb.st_blocks;
	attr.fileid = sb.st_ino;
	attr.atime_sec = sb.PORT_ST_ATIM.tv_sec;
	attr.atime_usec = sb.PORT_ST_ATIM.tv_nsec / 1000;
	attr.mtime_sec = sb.PORT_ST_MTIM.tv_sec;
	attr.mtime_usec = sb.PORT_ST_MTIM.tv_nsec / 1000;
	attr.ctime_sec = sb.PORT_ST_CTIM.tv_sec;
	attr.ctime_usec = sb.PORT_ST_CTIM.tv_nsec / 1000;
	}

	/* Encode reply: status, fh, fattr */
	xdr_put_u32(&r->out, NFS_OK);
	fh_encode(&dir_fh, &fh);
	/* Cache the path for READLINK (fhopen follows symlinks) */
	fh_path_cache_add(&dir_fh, fullpath);
	enc_fh(&r->out, &dir_fh);
	nfs2_enc_fattr(&r->out, &attr);
	log_msg(L_DEBUG, "LOOKUP: reply ok pos=%zu", xdr_pos(&r->out));
	return PROC_OK;
}

static int
nfs2_readlink(struct req *r)
{
	char cbuf[MAX_PATH_LEN];
	struct nfs_fh nfh;
	fhandle_t fh;
	char buf[1024];
	ssize_t n;
	const char *cpath;
	struct stat sb;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Path from LOOKUP (cache first: fhopen follows symlinks) */
	if (fh_resolve_path(&nfh, cbuf, sizeof(cbuf)) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	cpath = cbuf;

	if (lstat(cpath, &sb) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	if (!S_ISLNK(sb.st_mode)) {
	xdr_put_u32(&r->out, NFSERR_NXIO);
	return PROC_OK;
	}
	n = readlink(cpath, buf, sizeof(buf) - 1);
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	buf[n] = '\0';
	xdr_put_u32(&r->out, NFS_OK);
	xdr_put_string(&r->out, buf);
	return PROC_OK;
}

static int
nfs2_read(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	uint32_t offset, count;
	unsigned char buf[NFS2_MAXDATA];
	int fd;
	ssize_t n;
	uint32_t nstat;
	struct fs_fattr attr;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	offset = xdr_get_u32(&r->in);
	count = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (count > NFS2_MAXDATA)
	count = NFS2_MAXDATA;

	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	nstat = fs_getattr(&fh, &attr);
	if (nstat != NFS_OK) {
	xdr_put_u32(&r->out, nstat);
	return PROC_OK;
	}
	if (!S_ISREG(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	return PROC_OK;
	}
	/* Check read permission */
	if (fs_access(&attr, r->uid, r->gid, 0400) < 0) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}

	fd = PORT_FHOPEN(&fh, O_RDONLY);
	if (fd < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	(void)lseek(fd, offset, SEEK_SET);
	n = read(fd, buf, count);
	(void)close(fd);
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs2_enc_fattr(&r->out, &attr);
	xdr_put_u32(&r->out, (uint32_t)n);
	xdr_put_fixed(&r->out, buf, (size_t)n);
	/* pad to 4-byte boundary */
	{ size_t rem = (4 - ((size_t)n & 3)) & 3;
	xdr_put_fixed(&r->out, &nfs_pad, rem);
	}
	log_msg(L_DEBUG, "READ: ok n=%d pos=%zu", (int)n, xdr_pos(&r->out));
	return PROC_OK;
}

static int
nfs2_writecache(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFS_OK);
	return PROC_OK;
}

/* Helper: resolve directory path from fh */
static int
nfs2_resolve_dirpath(const struct nfs_fh *nfh, fhandle_t *fh, char *dirpath, size_t sz)
{
	if (fh_decode(nfh, fh) < 0)
	return -1;
	return fh_resolve_dirpath(nfh, dirpath, sz);
}

static int
nfs2_setattr(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	uint32_t mode, uid, gid, size;
	uint32_t atime_s, atime_u, mtime_s, mtime_u;
	const char *cpath;
	struct fs_fattr attr;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	mode = xdr_get_u32(&r->in);
	uid = xdr_get_u32(&r->in);
	gid = xdr_get_u32(&r->in);
	size = xdr_get_u32(&r->in);
	atime_s = xdr_get_u32(&r->in); atime_u = xdr_get_u32(&r->in);
	mtime_s = xdr_get_u32(&r->in); mtime_u = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	cpath = fh_path_cache_get(&nfh);
	if (cpath == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	{
	struct fs_setattr sa;
	struct stat sb;
	struct fs_fattr post;
	int serr;

	memset(&sa, 0, sizeof(sa));
	sa.have_mode = (mode != (uint32_t)-1); sa.mode = mode;
	sa.have_uid = (uid != (uint32_t)-1); sa.uid = uid;
	sa.have_gid = (gid != (uint32_t)-1); sa.gid = gid;
	sa.have_size = (size != (uint32_t)-1); sa.size = size;
	sa.atime_mode = (atime_s != (uint32_t)-1) ? 2 : 0;
	sa.atime_sec = atime_s; sa.atime_usec = (int32_t)atime_u;
	sa.mtime_mode = (mtime_s != (uint32_t)-1) ? 2 : 0;
	sa.mtime_sec = mtime_s; sa.mtime_usec = (int32_t)mtime_u;
	serr = fs_setattr_path(cpath, &sa);
	if (serr != 0) {
	xdr_put_u32(&r->out, nfs_errno((uint32_t)serr));
	return PROC_OK;
	}

	/* attrstat: status + fattr (RFC 1094) */
	if (lstat(cpath, &sb) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	memset(&post, 0, sizeof(post));
	post.mode = sb.st_mode;
	post.nlink = sb.st_nlink;
	post.uid = sb.st_uid;
	post.gid = sb.st_gid;
	post.size = sb.st_size;
	post.used = sb.st_blocks;
	post.fileid = sb.st_ino;
	post.atime_sec = sb.PORT_ST_ATIM.tv_sec;
	post.atime_usec = sb.PORT_ST_ATIM.tv_nsec / 1000;
	post.mtime_sec = sb.PORT_ST_MTIM.tv_sec;
	post.mtime_usec = sb.PORT_ST_MTIM.tv_nsec / 1000;
	post.ctime_sec = sb.PORT_ST_CTIM.tv_sec;
	post.ctime_usec = sb.PORT_ST_CTIM.tv_nsec / 1000;
	xdr_put_u32(&r->out, NFS_OK);
	nfs2_enc_fattr(&r->out, &post);
	return PROC_OK;
	}
}

static int
nfs2_write(struct req *r)
{
	char cbuf[MAX_PATH_LEN];
	struct nfs_fh nfh;
	fhandle_t fh;
	uint32_t offset, count;
	int fd;
	ssize_t n;
	const char *cpath;
	uint8_t *wbuf;
	size_t wsize;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	log_msg(L_DEBUG, "WRITE: no export");
	xdr_put_u32(&r->out, NFSERR_STALE);
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	log_msg(L_DEBUG, "WRITE: fh_decode failed");
	xdr_put_u32(&r->out, NFSERR_STALE);
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	/* NFSv2 WRITE request: fh + beginoffset(4) + offset(4) + totalcount(4) + data_len(4) + data */
	(void)xdr_get_u32(&r->in);  /* beginoffset */
	offset = xdr_get_u32(&r->in);
	(void)xdr_get_u32(&r->in);  /* totalcount */
	count = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	cpath = (fh_resolve_path(&nfh, cbuf, sizeof(cbuf)) == 0) ? cbuf : NULL;
	if (cpath == NULL) {
	log_msg(L_DEBUG, "WRITE: still no path");
	xdr_put_u32(&r->out, NFSERR_STALE);
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	if (fh_export_ro(&nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	wsize = xdr_left(&r->in);
	if (wsize < count)
	count = (uint32_t)wsize;
	wbuf = r->in.base + r->in.pos;

	fd = open(cpath, O_WRONLY | O_NOFOLLOW);
	if (fd < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	(void)lseek(fd, offset, SEEK_SET);
	log_msg(L_DEBUG, "WRITE: writing %d bytes at offset %d to %s", (int)count, (int)offset, cpath);
	n = write(fd, wbuf, count);
	log_msg(L_DEBUG, "WRITE: wrote %d errno=%d", (int)n, errno);
	/* NFSv2 has no stable_how concept - never fsync */
	(void)close(fd);
	if (n < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	xdr_put_u32(&r->out, 0); xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	xdr_put_u32(&r->out, (uint32_t)n);
	xdr_put_u32(&r->out, 0);  /* committed (unused in v2) */
	/* Encode fattr after write */
	{
	struct fs_fattr attr;
	if (fs_getattr(&fh, &attr) == NFS_OK)
	nfs2_enc_fattr(&r->out, &attr);
	}
	return PROC_OK;
}

static int
nfs2_create(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	char name[256];
	char dirpath[512], fullpath[512];
	uint32_t mode, uid, gid, size;
	uint32_t atime_s, atime_u, mtime_s, mtime_u;
	fhandle_t fh;
	struct nfs_fh new_nfh;
	int fd;

	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	mode = xdr_get_u32(&r->in);
	uid = xdr_get_u32(&r->in);
	gid = xdr_get_u32(&r->in);
	size = xdr_get_u32(&r->in);
	atime_s = xdr_get_u32(&r->in); atime_u = xdr_get_u32(&r->in);
	mtime_s = xdr_get_u32(&r->in); mtime_u = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	(void)size; (void)atime_s; (void)atime_u; (void)mtime_s; (void)mtime_u;

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	fd = open(fullpath, O_CREAT | O_RDWR | O_EXCL, mode);
	if (fd < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	(void)close(fd);
	if (uid != (uint32_t)-1 || gid != (uint32_t)-1)
	(void)chown(fullpath, uid == (uint32_t)-1 ? -1 : uid,
	    gid == (uint32_t)-1 ? -1 : gid);

	if (port_lgetfh(fullpath, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	fh_encode(&new_nfh, &fh);
	fh_path_cache_add(&new_nfh, fullpath);

	xdr_put_u32(&r->out, NFS_OK);
	enc_fh(&r->out, &new_nfh);
	{
	struct fs_fattr attr;
	if (fs_getattr(&fh, &attr) == NFS_OK)
	nfs2_enc_fattr(&r->out, &attr);
	}
	log_msg(L_DEBUG, "CREATE: %s ok", fullpath);
	return PROC_OK;
}

static int
nfs2_remove(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	char name[256];
	char dirpath[512], fullpath[512];

	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (fs_unlink(fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	log_msg(L_DEBUG, "REMOVE: %s ok", fullpath);
	return PROC_OK;
}

static int
nfs2_rename(struct req *r)
{
	struct nfs_fh from_nfh, to_nfh;
	fhandle_t from_fh, to_fh;
	char from_name[256], to_name[256];
	char from_path[512], to_path[512];
	char from_full[512], to_full[512];

	if (dec_fh(&r->in, &from_nfh) < 0)
	return PROC_GARBAGE;
	if (dec_fh(&r->in, &to_nfh) < 0)
	return PROC_GARBAGE;
	if (nfs_get_name(&r->in, from_name, sizeof(from_name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	from_name[sizeof(from_name)-1] = '\0';
	if (nfs_get_name(&r->in, to_name, sizeof(to_name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	to_name[sizeof(to_name)-1] = '\0';

	if (fh_lookup_export(&from_nfh) == NULL || fh_lookup_export(&to_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&from_nfh) || fh_export_ro(&to_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs2_resolve_dirpath(&from_nfh, &from_fh, from_path, sizeof(from_path)) < 0 ||
	    nfs2_resolve_dirpath(&to_nfh, &to_fh, to_path, sizeof(to_path)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(from_full, sizeof(from_full), "%s/%s", from_path, from_name);
	snprintf(to_full, sizeof(to_full), "%s/%s", to_path, to_name);

	if (fs_rename(from_full, to_full) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	log_msg(L_DEBUG, "RENAME: %s -> %s ok", from_full, to_full);
	return PROC_OK;
}

static int
nfs2_link(struct req *r)
{
	struct nfs_fh file_nfh, dir_nfh;
	fhandle_t dir_fh;
	char name[256];
	char dirpath[512], fullpath[512];
	const char *oldpath;

	if (dec_fh(&r->in, &file_nfh) < 0)
	return PROC_GARBAGE;
	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&file_nfh) == NULL || fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&file_nfh) || fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	oldpath = fh_path_cache_get(&file_nfh);
	if (oldpath == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (link(oldpath, fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	log_msg(L_DEBUG, "LINK: %s -> %s ok", oldpath, fullpath);
	return PROC_OK;
}

static int
nfs2_symlink(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	char target[1024], name[256];
	char dirpath[512], fullpath[512];

	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, target, sizeof(target) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	target[sizeof(target)-1] = '\0';

	/* Skip sattr: mode+uid+gid+size+atime(2)+mtime(2) = 32 bytes */
	(void)xdr_get_u32(&r->in); /* mode */
	(void)xdr_get_u32(&r->in); /* uid */
	(void)xdr_get_u32(&r->in); /* gid */
	(void)xdr_get_u32(&r->in); /* size */
	(void)xdr_get_u32(&r->in); (void)xdr_get_u32(&r->in); /* atime */
	(void)xdr_get_u32(&r->in); (void)xdr_get_u32(&r->in); /* mtime */
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (symlink(target, fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	log_msg(L_DEBUG, "SYMLINK: %s -> %s ok", fullpath, target);
	return PROC_OK;
}

static int
nfs2_mkdir(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	char name[256];
	char dirpath[512], fullpath[512];
	uint32_t mode, uid, gid, size;
	uint32_t atime_s, atime_u, mtime_s, mtime_u;
	fhandle_t fh;
	struct nfs_fh new_nfh;

	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	mode = xdr_get_u32(&r->in);
	uid = xdr_get_u32(&r->in);
	gid = xdr_get_u32(&r->in);
	size = xdr_get_u32(&r->in);
	atime_s = xdr_get_u32(&r->in); atime_u = xdr_get_u32(&r->in);
	mtime_s = xdr_get_u32(&r->in); mtime_u = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	(void)size; (void)atime_s; (void)atime_u; (void)mtime_s; (void)mtime_u;

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (mkdir(fullpath, mode) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	if (uid != (uint32_t)-1 || gid != (uint32_t)-1)
	(void)chown(fullpath, uid == (uint32_t)-1 ? -1 : uid,
	    gid == (uint32_t)-1 ? -1 : gid);

	if (port_lgetfh(fullpath, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	fh_encode(&new_nfh, &fh);
	fh_path_cache_add(&new_nfh, fullpath);

	xdr_put_u32(&r->out, NFS_OK);
	enc_fh(&r->out, &new_nfh);
	{
	struct fs_fattr attr;
	if (fs_getattr(&fh, &attr) == NFS_OK)
	nfs2_enc_fattr(&r->out, &attr);
	}
	log_msg(L_DEBUG, "MKDIR: %s ok", fullpath);
	return PROC_OK;
}

static int
nfs2_rmdir(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	char name[256];
	char dirpath[512], fullpath[512];

	if (dec_fh(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (nfs_get_name(&r->in, name, sizeof(name) - 1, 0) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_export_ro(&dir_nfh)) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs2_resolve_dirpath(&dir_nfh, &dir_fh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (fs_rmdir(fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	log_msg(L_DEBUG, "RMDIR: %s ok", fullpath);
	return PROC_OK;
}

static int
nfs2_readdir(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	uint32_t offset, count;
	struct fs_fattr attr;
	uint32_t nstat;
	const struct dirlist *list;
	size_t i;

	if (dec_fh(&r->in, &nfh) < 0) {
	log_msg(L_DEBUG, "READDIR: dec_fh failed");
	return PROC_GARBAGE;
	}
	if (fh_lookup_export(&nfh) == NULL) {
	log_msg(L_DEBUG, "READDIR: no export");
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	offset = xdr_get_u32(&r->in);
	count = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in)) {
	log_msg(L_DEBUG, "READDIR: xdr_ok failed");
	return PROC_GARBAGE;
	}
	if (count > NFS2_MAXRDIR)
	count = NFS2_MAXRDIR;

	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	nstat = fs_getattr(&fh, &attr);
	log_msg(L_DEBUG, "READDIR: fs_getattr=%d mode=%o", nstat, (unsigned)attr.mode);
	if (nstat != NFS_OK) {
xdr_put_u32(&r->out, nstat);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	if (!S_ISDIR(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_NOTDIR);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	log_msg(L_DEBUG, "READDIR: checking access uid=%lu gid=%lu mode=%o",
	    (unsigned long)r->uid, (unsigned long)r->gid, (unsigned)attr.mode);
	if (fs_access(&attr, r->uid, r->gid, 0500) < 0) {
	log_msg(L_DEBUG, "READDIR: access denied");
	xdr_put_u32(&r->out, NFSERR_ACCES);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	/*
	 * "offset" is the cookie of the last entry the client got (0 = start).
	 * Cookies come from the entry names (see dirlist.h). They are not
	 * positions and not inode numbers: the old code sent inode numbers as
	 * cookies but used the next "offset" as the number of entries to skip.
	 * A big directory was cut off after the first batch.
	 */
	list = dircache_get(&nfh, &fh, &attr, offset, DL_BITS_V2);
	if (list == NULL) {
	xdr_put_u32(&r->out, NFSERR_IO);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	/*
	 * NFSv2 READDIR reply (RFC 1094): status, then for each entry
	 *   more=1, fileid, name, cookie
	 * then more=0 and eof. The handle of an entry is not part of it;
	 * the client uses LOOKUP.
	 */
	xdr_put_u32(&r->out, NFS_OK);
	for (i = dl_after(list, offset); i < list->n; i++) {
	const struct dl_entry *de = &list->e[i];
	size_t entry_size = 4 + 4 + 4 + XDR_PAD(strlen(de->name)) + 4;

	/* leave room for the end of the list: more=0 and eof */
	if (xdr_pos(&r->out) + entry_size + 8 > count)
	break;
	xdr_put_u32(&r->out, 1);
	xdr_put_u32(&r->out, (uint32_t)de->inode);
	xdr_put_string(&r->out, de->name);
	xdr_put_u32(&r->out, (uint32_t)de->cookie);
	}
	xdr_put_u32(&r->out, 0);		/* more = 0 */
	xdr_put_u32(&r->out, i >= list->n);	/* eof */
	log_msg(L_DEBUG, "READDIR: cookie=%u eof=%d", (unsigned)offset,
	    i >= list->n);
	return PROC_OK;
}

static int
nfs2_statfs(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct statfs sf;
	uint32_t nstat;

	if (dec_fh(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	nstat = fs_statfs(&fh, &sf);
	if (nstat != 0) {
	xdr_put_u32(&r->out, nfs_errno(nstat));
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs2_enc_statfs(&r->out, &sf);
	return PROC_OK;
}
