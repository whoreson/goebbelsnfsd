#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "progs.h"
#include "rpc.h"
#include "log.h"
#include "xdr.h"
#include "fh.h"
#include "fs.h"
#include "conf.h"
#include "nfs_common.h"

#define NFS3_MAXDATA	32768

/* NFSv3 write verifier (8 bytes) */
uint32_t nfs3_wverf[2];
#define NFS3_MAXRDIR	32768

static int nfs3_null(struct req *r) { (void)r; return PROC_OK; }
static int __attribute__((unused)) nfs3_notimpl(struct req *r) { (void)r; return PROC_UNAVAIL; }

/* NFSv3 file handle: opaque <NFSX_V3FHMAX> (len prefix + data) */
static void
enc_fh3(struct xdr *x, struct nfs_fh *nfh)
{
	xdr_put_var(x, nfh, NFS_FH_SIZE);
}

static void
enc_postop_fh3(struct xdr *x, struct nfs_fh *nfh)
{
	xdr_put_u32(x, 1);  /* handle_follows = true */
	xdr_put_var(x, nfh, NFS_FH_SIZE);
}

static int
dec_fh3(struct xdr *x, struct nfs_fh *nfh)
{
	size_t n = xdr_get_var(x, nfh, NFS_FH_SIZE);
	if (n != NFS_FH_SIZE || !xdr_ok(x))
	return -1;
	return 0;
}

/*
 * post_op_attr: attr_follows(1) + fattr(if true)
 */
/* Encode post_op_attr from a pre-computed fattr (avoid re-stat) */
static void
enc_postop_attr_fattr(struct xdr *x, const struct fs_fattr *attr)
{
	if (attr == NULL) {
	xdr_put_u32(x, 0);
	return;
	}
	xdr_put_u32(x, 1);
	nfs3_enc_fattr(x, attr);
}

/* Encode post_op_attr by decoding fh and getting attributes */
static void
enc_postop_attr(struct xdr *x, const struct nfs_fh *nfh)
{
	if (nfh == NULL) {
	xdr_put_u32(x, 0);
	return;
	}
	fhandle_t fh;
	struct fs_fattr attr;
	int ok = fh_decode(nfh, &fh) >= 0 && fs_getattr(&fh, &attr) == NFS_OK;
	log_msg(L_DEBUG, "enc_postop_attr: ok=%d", ok);
	if (ok) {
	xdr_put_u32(x, 1);
	nfs3_enc_fattr(x, &attr);
	} else {
	xdr_put_u32(x, 0);
	}
}

/* GETATTR */
static int
nfs3_getattr(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr attr;
	uint32_t nstat;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	nstat = fs_getattr(&fh, &attr);
	if (nstat != NFS_OK) {
	xdr_put_u32(&r->out, nstat);
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_fattr(&r->out, &attr);
	return PROC_OK;
}

/* SETATTR - read-only, always fail */
static int
nfs3_setattr(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr pre_attr, post_attr;
	const struct export *ex;
	const char *cpath;
	int e;
	uint32_t mode_set, uid_set, gid_set, size_set;
	uint32_t atime_set, mtime_set;
	uint32_t guard_size_set, guard_mtime_set;
	uint32_t new_mode, new_uid, new_gid;
	uint64_t new_size;
	uint64_t atime_s, atime_ns, mtime_s, mtime_ns;
	uint64_t guard_mtime_s, guard_mtime_ns, guard_size;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	log_msg(L_DEBUG, "nfs3_setattr: fh3 decoded, pos=%zu len=%zu", r->in.pos, r->in.len);
	ex = fh_lookup_export(&nfh);
	log_msg(L_DEBUG, "nfs3_setattr: ex=%p ro=%d", (void*)ex, ex ? ex->ro : -1);
	if (ex == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (ex->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	/* Decode sattr3 (each field has bool_t discriminant) */
	mode_set = xdr_get_u32(&r->in); if (mode_set) new_mode = xdr_get_u32(&r->in);
	uid_set = xdr_get_u32(&r->in); if (uid_set) new_uid = xdr_get_u32(&r->in);
	gid_set = xdr_get_u32(&r->in); if (gid_set) new_gid = xdr_get_u32(&r->in);
	size_set = xdr_get_u32(&r->in); if (size_set) new_size = xdr_get_u64(&r->in);
	atime_set = xdr_get_u32(&r->in);
	if (atime_set == 2) { atime_s = xdr_get_u32(&r->in); atime_ns = xdr_get_u32(&r->in); }
	else if (atime_set == 1) (void)xdr_get_u32(&r->in);  /* TOSERVER: skip 1 extra u32 */
	mtime_set = xdr_get_u32(&r->in);
	if (mtime_set == 2) { mtime_s = xdr_get_u32(&r->in); mtime_ns = xdr_get_u32(&r->in); }
	else if (mtime_set == 1) (void)xdr_get_u32(&r->in);  /* TOSERVER: skip 1 extra u32 */

	/* Decode sattrguard3 - FreeBSD 8 client sends only obj_size discriminant */
	guard_size_set = xdr_get_u32(&r->in); if (guard_size_set) guard_size = xdr_get_u64(&r->in);
	/* Client does not send guard mtime */
	guard_mtime_set = 0;

	if (!xdr_ok(&r->in)) {
	log_msg(L_DEBUG, "nfs3_setattr: xdr_ok failed, pos=%zu len=%zu mode=%d uid=%d gid=%d size=%d atime=%d mtime=%d gsz=%d gmt=%d",
	    r->in.pos, r->in.len,
	    mode_set, uid_set, gid_set, size_set, atime_set, mtime_set,
	    guard_size_set, guard_mtime_set);
	return PROC_GARBAGE;
	}
	log_msg(L_DEBUG, "nfs3_setattr: sattr3 decoded ok, mode=%d uid=%d gid=%d size=%d atime=%d mtime=%d",
	    mode_set, uid_set, gid_set, size_set, atime_set, mtime_set);

	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Guard check */
	if (guard_size_set && pre_attr.size != guard_size) {
	xdr_put_u32(&r->out, NFSERR_GARBAGE);
	return PROC_OK;
	}
	if (guard_mtime_set) {
	if ((uint64_t)pre_attr.mtime_sec != guard_mtime_s ||
	    (uint64_t)pre_attr.mtime_usec * 1000 != guard_mtime_ns) {
	xdr_put_u32(&r->out, NFSERR_GARBAGE);
	return PROC_OK;
	}
	}

	/* Get path for operations */
	cpath = fh_path_cache_get(&nfh);
	log_msg(L_DEBUG, "nfs3_setattr: cpath=%s", cpath ? cpath : "(null)");
	if (cpath == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	log_msg(L_DEBUG, "nfs3_setattr: path=%s mode_set=%d uid_set=%d gid_set=%d size_set=%d atime_set=%d mtime_set=%d",
	    cpath, mode_set, uid_set, gid_set, size_set, atime_set, mtime_set);

	/* Apply changes */
	if (size_set) {
	e = truncate(cpath, (off_t)new_size);
	if (e < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	}
	if (mode_set) {
	e = chmod(cpath, new_mode);
	if (e < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	}
	if (uid_set || gid_set) {
	e = lchown(cpath, uid_set ? new_uid : -1, gid_set ? new_gid : -1);
	if (e < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	}
	if (atime_set || mtime_set) {
	struct timeval tv[2];
	struct timeval now;
	gettimeofday(&now, NULL);
	tv[0].tv_sec = (time_t)(atime_set == 2 ? atime_s : (atime_set == 1 ? now.tv_sec : pre_attr.atime_sec));
	tv[0].tv_usec = (suseconds_t)(atime_set == 2 ? atime_ns / 1000 : (atime_set == 1 ? now.tv_usec : pre_attr.atime_usec));
	tv[1].tv_sec = (time_t)(mtime_set == 2 ? mtime_s : (mtime_set == 1 ? now.tv_sec : pre_attr.mtime_sec));
	tv[1].tv_usec = (suseconds_t)(mtime_set == 2 ? mtime_ns / 1000 : (mtime_set == 1 ? now.tv_usec : pre_attr.mtime_usec));
	e = utimes(cpath, tv);
	/* utimes errors are not fatal for setattr */
	(void)e;
	}

	/* Get post-attr */
	if (fs_getattr(&fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	return PROC_OK;
}

/* LOOKUP */
static int
nfs3_lookup(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh, fh_child;
	struct nfs_fh child_nfh;
	char name[256];
	char dirpath[512];
	char fullpath[512];
	int dfd;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
	}

	/* Resolve directory path from file handle */
	dfd = fhopen(&fh, O_RDONLY);
	if (dfd < 0 || fchdir(dfd) < 0 || getcwd(dirpath, sizeof(dirpath)) == NULL) {
	(void)close(dfd);
	xdr_put_u32(&r->out, NFSERR_IO);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
	}
	(void)close(dfd);
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (lgetfh(fullpath, &fh_child) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
	}
	fh_encode(&child_nfh, &fh_child);
	fh_path_cache_add(&child_nfh, fullpath);

	xdr_put_u32(&r->out, NFS_OK);
	enc_fh3(&r->out, &child_nfh);
	enc_postop_attr(&r->out, &child_nfh);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
}

/* ACCESS */
static int
nfs3_access(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr attr;
	uint32_t req_access, granted;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	req_access = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* NFSv3 access bits: READ(1), LOOKUP(2), MODIFY(4), ADD(8), DELETE(16), EXECUTE(32) */
	granted = 0;
	const struct export *ex = fh_lookup_export(&nfh);
	int rw = (ex != NULL && !ex->ro);
	if (req_access & 1 && (attr.mode & 0400))
	granted |= 1;
	if (req_access & 2 && S_ISDIR(attr.mode) && (attr.mode & 0100))
	granted |= 2;
	if (rw) {
	if (req_access & 4 && (attr.mode & 0200))
	granted |= 4;
	if (req_access & 8) {
	if (S_ISDIR(attr.mode)) {
	if ((attr.mode & 0200) && (attr.mode & 0100))
	granted |= 8;
	} else if (attr.mode & 0200) {
	granted |= 8;
	}
	}
	if (req_access & 16 && S_ISDIR(attr.mode) && (attr.mode & 0200) && (attr.mode & 0100))
	granted |= 16;
	}
	if (req_access & 32 && (attr.mode & 0100))
	granted |= 32;
	log_msg(L_DEBUG, "nfs3_access: req=%d granted=%d mode=%o rw=%d", req_access, granted, attr.mode, rw);

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, granted);
	return PROC_OK;
}

/* READLINK */
static int
nfs3_readlink(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr attr;
	char link[1024];
	ssize_t n;
	const char *cpath;
	struct stat sb;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Use path cache to detect symlinks */
	cpath = fh_path_cache_get(&nfh);
	if (cpath != NULL && lstat(cpath, &sb) == 0) {
	if (!S_ISLNK(sb.st_mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	n = readlink(cpath, link, sizeof(link) - 1);
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	} else {
	if (!S_ISLNK(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	n = fs_readlink(&fh, link, sizeof(link));
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	}

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_var(&r->out, link, (size_t)n);
	return PROC_OK;
}

/* READ */
static int
nfs3_read(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr attr;
	uint64_t offset;
	uint32_t count;
	int fd;
	ssize_t n;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	offset = xdr_get_u64(&r->in);
	count = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (count > NFS3_MAXDATA)
	count = NFS3_MAXDATA;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (S_ISDIR(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_ISDIR);
	return PROC_OK;
	}
	if (r->uid != 0 && (attr.mode & 0400) == 0) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}

	fd = fs_open(&fh, 0);
	if (fd < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
	(void)close(fd);
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}

	/* Read one extra byte to check for EOF */
	{
	uint8_t *tmp = malloc(count + 1);
	if (tmp == NULL) {
	(void)close(fd);
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	n = read(fd, tmp, count + 1);
	if (n < 0)
	n = 0;
	(void)close(fd);
	int is_eof = (n <= (int)count);
	if ((size_t)n > count)
	n = count;

	/* reply: status + postop_attr + count + eof + data_len + data */
	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	{
	size_t count_mark = xdr_pos(&r->out);
	xdr_put_u32(&r->out, 0);  /* count, patch later */
	xdr_put_u32(&r->out, is_eof ? 1 : 0);  /* eof */
	xdr_put_u32(&r->out, (uint32_t)n);  /* data_len */
	xdr_put_fixed(&r->out, tmp, (size_t)n);  /* data */
	xdr_patch_u32(&r->out, count_mark, (uint32_t)n);  /* patch count */
	}
	free(tmp);
	log_msg(L_DEBUG, "nfs3_read: done n=%d eof=%d", (int)n, is_eof);
	}
	return PROC_OK;
}

/* READDIR */
static int
nfs3_readdir(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	uint64_t cookie;
	uint32_t count;
	DIR *dirp;
	uint64_t inode;
	char name[256];
	int rc, done = 0;
	struct fs_fattr attr;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	cookie = xdr_get_u64(&r->in);
	(void)xdr_get_u32(&r->in);  /* cookieverf[0] */
	(void)xdr_get_u32(&r->in);  /* cookieverf[1] */
	count = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (count > NFS3_MAXRDIR)
	count = NFS3_MAXRDIR;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (!S_ISDIR(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_NOTDIR);
	return PROC_OK;
	}

	if (fs_opendir(&fh, &dirp) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}

	/* Skip to cookie */
	{
	uint64_t cur = 0;
	while (cur < cookie) {
	rc = fs_readdir(dirp, &inode, name, sizeof(name));
	if (rc != 0) {
	done = 1;
	break;
	}
	cur++;
	}
	}

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);  /* cookieverf */
	xdr_put_u32(&r->out, 0);

	/* Write entries (linked list with NULL terminator) */
	{
	uint64_t cur = cookie;
	while (!done) {
	rc = fs_readdir(dirp, &inode, name, sizeof(name));
	if (rc != 0) {
	done = 1;
	continue;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
	cur++;
	continue;
	}

	{
	size_t namelen = strlen(name);
	/* FreeBSD 8 client expects: nextentry(4) + fileid_high(4) + fileid_low(4) + namelen(4) + name(padded) + cookie_high(4) + cookie_low(4) */
	size_t entry_size = 4 + 4 + 4 + 4 + XDR_PAD(namelen) + 4 + 4;
	if (xdr_pos(&r->out) + entry_size > count + 24) {
	done = 1;
	break;
	}
	/* nextentry = true */
	xdr_put_u32(&r->out, 1);
	/* fileid as 64-bit (high=0, low=inode) */
	xdr_put_u32(&r->out, 0);
	xdr_put_u32(&r->out, (uint32_t)inode);
	/* name */
	xdr_put_string(&r->out, name);
	/* cookie as 64-bit (high=0, low=cur) */
	xdr_put_u32(&r->out, 0);
	xdr_put_u32(&r->out, (uint32_t)cur);
	}
	cur++;
	}
	}

	/* nextentry = false */
	xdr_put_u32(&r->out, 0);

	/* eof */
	xdr_put_u32(&r->out, done ? 1 : 0);

	(void)closedir(dirp);
	return PROC_OK;
}

/* READDIRPLUS - return NOTSUPP to fall back to READDIR */
static int
nfs3_readdirplus(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_NOTSUPP);
	return PROC_OK;
}

/* FSSTAT */
static int
nfs3_fsstat(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct statfs sf;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fs_statfs(&fh, &sf) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u64(&r->out, (uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize);
	xdr_put_u64(&r->out, (uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize);
	xdr_put_u64(&r->out, (uint64_t)sf.f_bfree * (uint64_t)sf.f_bsize);
	xdr_put_u64(&r->out, (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize);
	xdr_put_u64(&r->out, (uint64_t)sf.f_files);
	xdr_put_u64(&r->out, (uint64_t)sf.f_ffree);
	xdr_put_u64(&r->out, (uint64_t)sf.f_ffree);
	xdr_put_u32(&r->out, sf.f_bsize);
	xdr_put_u32(&r->out, 60 * 60);
	xdr_put_u32(&r->out, 60);
	return PROC_OK;
}

/* FSINFO */
static int
nfs3_fsinfo(struct req *r)
{
	struct nfs_fh nfh;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* rtmax */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* rtpref */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* rtmult */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* wtmax */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* wtpref */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* wtmult */
	xdr_put_u32(&r->out, NFS3_MAXDATA);  /* dtpref */
	xdr_put_u64(&r->out, 1ULL << 32);    /* maxfilesize */
	xdr_put_u32(&r->out, 1);             /* time_delta seconds */
	xdr_put_u32(&r->out, 0);             /* time_delta nseconds */
	xdr_put_u32(&r->out, 0);             /* properties */
	return PROC_OK;
}

/* PATHCONF */
static int
nfs3_pathconf(struct req *r)
{
	struct nfs_fh nfh;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 32767);
	xdr_put_u32(&r->out, 255);
	xdr_put_u32(&r->out, 0);
	xdr_put_u32(&r->out, 1);
	xdr_put_u32(&r->out, 0);
	xdr_put_u32(&r->out, 1);
	return PROC_OK;
}

/* Helper: check export is writable, return NULL if stale, 1 if ro, else export */
static const struct export *
nfs3_check_export_rw(const struct nfs_fh *nfh)
{
	const struct export *ex = fh_lookup_export(nfh);
	if (ex == NULL)
	return NULL;
	if (ex->ro)
	return (const struct export *)1;
	return ex;
}

/* Helper: skip remaining sattr3 fields (size, atime, mtime) */
static void
nfs3_skip_sattr3_rest(struct xdr *x)
{
	uint32_t v;
	v = xdr_get_u32(x); if (v) (void)xdr_get_u64(x);  /* size */
	v = xdr_get_u32(x); if (v == 2) { (void)xdr_get_u32(x); (void)xdr_get_u32(x); } else if (v == 1) (void)xdr_get_u32(x);  /* atime: TOCLIENT=2 sends 8 bytes, TOSERVER=1 sends 4 bytes */
	v = xdr_get_u32(x); if (v == 2) { (void)xdr_get_u32(x); (void)xdr_get_u32(x); } else if (v == 1) (void)xdr_get_u32(x);  /* mtime: same */
	(void)v;
}

/* Helper: decode sattr3 fields (mode/uid/gid), skip the rest */
static void
nfs3_decode_sattr3_mode_uid_gid(struct xdr *x, uint32_t *mode, uint32_t *uid, uint32_t *gid, int *uid_set, int *gid_set)
{
	uint32_t v;
	*uid_set = 0; *gid_set = 0;
	v = xdr_get_u32(x); if (v) *mode = xdr_get_u32(x); else *mode = 0666;
	v = xdr_get_u32(x); if (v) { *uid = xdr_get_u32(x); *uid_set = 1; }
	v = xdr_get_u32(x); if (v) { *gid = xdr_get_u32(x); *gid_set = 1; }
	nfs3_skip_sattr3_rest(x);  /* skip remaining: size, atime, mtime */
}

/* Helper: resolve parent dir path from fh */
static int
nfs3_resolve_dirpath(const struct nfs_fh *nfh, char *dirpath, size_t dirpathsz)
{
	fhandle_t fh;
	int dfd;

	if (fh_decode(nfh, &fh) < 0)
	return -1;
	dfd = fhopen(&fh, O_RDONLY);
	if (dfd < 0 || fchdir(dfd) < 0 || getcwd(dirpath, dirpathsz) == NULL) {
	(void)close(dfd);
	return -1;
	}
	(void)close(dfd);
	return 0;
}

/* Helper: encode wcc_data using pre-computed fs_fattr for both pre and post */
static void
nfs3_enc_wcc_from_attr(struct xdr *x, const struct fs_fattr *pre, const struct fs_fattr *post)
{
	nfs3_enc_pre_op_attr(x, pre);
	enc_postop_attr_fattr(x, post);
}

/* WRITE */
static int
nfs3_write(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr pre_attr, post_attr;
	uint64_t offset;
	uint32_t count, stable_how;
	uint32_t dlen;
	const char *cpath;
	int fd;
	ssize_t n;
	uint8_t *buf;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	offset = xdr_get_u64(&r->in);
	count = xdr_get_u32(&r->in);
	stable_how = xdr_get_u32(&r->in);
	dlen = xdr_get_u32(&r->in);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (count > NFS3_MAXDATA)
	count = NFS3_MAXDATA;
	if (dlen > count)
	dlen = count;

	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Check write access */
	if (fh_lookup_export(&nfh)->ro) {
	/* consume data */
	uint8_t *skip = malloc(dlen);
	if (skip) { xdr_get_fixed(&r->in, skip, dlen); free(skip); }
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	cpath = fh_path_cache_get(&nfh);
	if (cpath == NULL) {
	uint8_t *skip = malloc(dlen);
	if (skip) { xdr_get_fixed(&r->in, skip, dlen); free(skip); }
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Consume data */
	buf = malloc(dlen);
	if (buf == NULL) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	xdr_get_fixed(&r->in, buf, dlen);

	fd = open(cpath, O_WRONLY);
	if (fd < 0) {
	free(buf);
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
	(void)close(fd);
	free(buf);
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	n = write(fd, buf, dlen);
	free(buf);
	if (n < 0) {
	(void)close(fd);
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}

	/* stable_how: FILESYNC(2) => fsync, DATASYNC(1) => fdatasync, UNSTABLE(0) => no sync */
	if (stable_how >= NFS3_MAXDATA) {
	/* treat FILESYNC and above as fsync */
	(void)fsync(fd);
	}
	(void)close(fd);

	if (fs_getattr(&fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	xdr_put_u32(&r->out, (uint32_t)n);  /* count */
	xdr_put_u32(&r->out, 2);  /* committed = FILESYNC */
	xdr_put_u32(&r->out, nfs3_wverf[0]);
	xdr_put_u32(&r->out, nfs3_wverf[1]);
	return PROC_OK;
}

/* COMMIT */
static int
nfs3_commit(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	struct fs_fattr pre_attr, post_attr;
	const char *cpath;
	int fd;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	(void)xdr_get_u64(&r->in);  /* offset */
	(void)xdr_get_u32(&r->in);  /* count */

	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	cpath = fh_path_cache_get(&nfh);
	if (cpath == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	fd = open(cpath, O_RDONLY);
	if (fd >= 0) {
	(void)fsync(fd);
	(void)close(fd);
	}

	if (fs_getattr(&fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	xdr_put_u32(&r->out, nfs3_wverf[0]);
	xdr_put_u32(&r->out, nfs3_wverf[1]);
	return PROC_OK;
}

/* CREATE */
static int
nfs3_create(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr;
	char name[256];
	char dirpath[512], fullpath[512];
	fhandle_t fh_child;
	struct nfs_fh child_nfh;
	int fd;
	uint32_t how_mode;

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';
	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Check write access */
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	/* Decode createhow3 */
	how_mode = xdr_get_u32(&r->in);
	/* Decode sattr3 fields */
	uint32_t new_mode, new_uid, new_gid;
	int uid_set, gid_set;
	nfs3_decode_sattr3_mode_uid_gid(&r->in, &new_mode, &new_uid, &new_gid, &uid_set, &gid_set);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (how_mode == 0) {  /* UNCHECKED */
	fd = open(fullpath, O_CREAT | O_RDWR | O_TRUNC, new_mode);
	} else if (how_mode == 1) {  /* GUARDED */
	fd = open(fullpath, O_CREAT | O_EXCL | O_RDWR | O_TRUNC, new_mode);
	} else {  /* EXCLUSIVE - simplified: same as GUARDED */
	fd = open(fullpath, O_CREAT | O_EXCL | O_RDWR | O_TRUNC, new_mode);
	}

	if (fd < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	(void)close(fd);

	/* Set uid/gid if provided */
	if (uid_set || gid_set) {
	(void)chown(fullpath, uid_set ? new_uid : -1, gid_set ? new_gid : -1);
	}

	if (lgetfh(fullpath, &fh_child) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	fh_encode(&child_nfh, &fh_child);
	fh_path_cache_add(&child_nfh, fullpath);

	if (fs_getattr(&fh_child, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	/* Get updated directory attributes for WCC */
	struct fs_fattr dir_post_attr;
	(void)fs_getattr(&dir_fh, &dir_post_attr);

	xdr_put_u32(&r->out, NFS_OK);
	/* CREATE3resok: obj(post_op_fh3) + obj_attr(post_op_attr) + dir_wcc(wcc_data) */
	enc_postop_fh3(&r->out, &child_nfh);
	enc_postop_attr_fattr(&r->out, &post_attr);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &dir_post_attr);
	return PROC_OK;
}

/* MKDIR */
static int
nfs3_mkdir(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr;
	char name[256];
	char dirpath[512], fullpath[512];
	fhandle_t fh_child;
	struct nfs_fh child_nfh;

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	/* Decode sattr3 */
	uint32_t new_mode = 0777, new_uid, new_gid;
	int uid_set, gid_set;
	nfs3_decode_sattr3_mode_uid_gid(&r->in, &new_mode, &new_uid, &new_gid, &uid_set, &gid_set);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);
	log_msg(L_DEBUG, "nfs3_mkdir: path=%s", fullpath);

	if (mkdir(fullpath, new_mode) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	/* Set uid/gid if provided */
	if (uid_set || gid_set) {
	(void)chown(fullpath, uid_set ? new_uid : -1, gid_set ? new_gid : -1);
	}

	if (lgetfh(fullpath, &fh_child) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	fh_encode(&child_nfh, &fh_child);
	fh_path_cache_add(&child_nfh, fullpath);

	if (fs_getattr(&fh_child, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	struct fs_fattr dir_post_attr;
	(void)fs_getattr(&dir_fh, &dir_post_attr);

	xdr_put_u32(&r->out, NFS_OK);
	/* MKDIR3resok: obj(post_op_fh3) + obj_attr(post_op_attr) + dir_wcc(wcc_data) */
	enc_postop_fh3(&r->out, &child_nfh);
	enc_postop_attr_fattr(&r->out, &post_attr);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &dir_post_attr);
	log_msg(L_DEBUG, "nfs3_mkdir: success");
	return PROC_OK;
}

/* SYMLINK */
static int
nfs3_symlink(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr;
	char target[1024], name[256];
	char dirpath[512], fullpath[512];
	fhandle_t fh_child;
	struct nfs_fh child_nfh;

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;

	/* Decode name (symlink name in directory) */
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	/* Decode sattr3 */
	uint32_t new_uid, new_gid;
	int uid_set, gid_set;
	{
	uint32_t dummy_mode;
	nfs3_decode_sattr3_mode_uid_gid(&r->in, &dummy_mode, &new_uid, &new_gid, &uid_set, &gid_set);
	}
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	/* Decode symlink target */
	if (xdr_get_string(&r->in, target, sizeof(target) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	target[sizeof(target)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);
	log_msg(L_DEBUG, "nfs3_symlink: path=%s target=%s", fullpath, target);

	if (symlink(target, fullpath) < 0) {
	log_msg(L_DEBUG, "nfs3_symlink: symlink failed: %s", strerror(errno));
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	/* Set uid/gid if provided (use lchown for symlinks) */
	if (uid_set || gid_set) {
	(void)lchown(fullpath, uid_set ? new_uid : -1, gid_set ? new_gid : -1);
	}

	if (lgetfh(fullpath, &fh_child) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	fh_encode(&child_nfh, &fh_child);
	fh_path_cache_add(&child_nfh, fullpath);

	if (fs_getattr(&fh_child, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	struct fs_fattr dir_post_attr;
	(void)fs_getattr(&dir_fh, &dir_post_attr);

	xdr_put_u32(&r->out, NFS_OK);
	/* SYMLINK3resok: obj(post_op_fh3) + obj_attr(post_op_attr) + dir_wcc(wcc_data) */
	enc_postop_fh3(&r->out, &child_nfh);
	enc_postop_attr_fattr(&r->out, &post_attr);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &dir_post_attr);
	log_msg(L_DEBUG, "nfs3_symlink: success");
	return PROC_OK;
}

/* MKNOD - create FIFO, socket, or device special files */
static int
nfs3_mknod(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr, dir_post_attr;
	char name[256];
	char dirpath[512], fullpath[512];
	fhandle_t fh_child;
	struct nfs_fh child_nfh;
	uint32_t type;
	uint32_t new_mode, new_uid, new_gid;
	int uid_set, gid_set;

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';
	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 ||
	    fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	type = xdr_get_u32(&r->in);
	/* Only FIFO(7) and SOCK(6) are supported without privs */
	if (type != 6 && type != 7) {
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
	}

	/* Decode sattr3 for pipe/socket attributes */
	nfs3_decode_sattr3_mode_uid_gid(&r->in, &new_mode, &new_uid, &new_gid,
	    &uid_set, &gid_set);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (type == 7) {
	/* FIFO */
	if (mkfifo(fullpath, new_mode) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	} else {
	/* UNIX socket: create, bind, close */
	int sock = socket(PF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr;
	socklen_t bindlen;

	if (sock < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, fullpath, sizeof(addr.sun_path));
	bindlen = SUN_LEN(&addr);
	if (bind(sock, (struct sockaddr *)&addr, bindlen) < 0) {
	(void)close(sock);
	(void)unlink(fullpath);
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}
	(void)close(sock);
	(void)chmod(fullpath, new_mode);
	}

	/* Set uid/gid if provided */
	if (uid_set || gid_set) {
	(void)chown(fullpath, uid_set ? new_uid : -1, gid_set ? new_gid : -1);
	}

	/* lgetfh fails on FIFOs/sockets; get attrs via stat */
	{
	struct stat st;
	if (stat(fullpath, &st) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	memset(&post_attr, 0, sizeof(post_attr));
	post_attr.mode = st.st_mode;
	post_attr.nlink = st.st_nlink;
	post_attr.uid = st.st_uid;
	post_attr.gid = st.st_gid;
	post_attr.size = st.st_size;
	post_attr.fileid = st.st_ino;
	post_attr.atime_sec = st.st_atimespec.tv_sec;
	post_attr.atime_usec = st.st_atimespec.tv_nsec / 1000;
	post_attr.mtime_sec = st.st_mtimespec.tv_sec;
	post_attr.mtime_usec = st.st_mtimespec.tv_nsec / 1000;
	post_attr.ctime_sec = st.st_ctimespec.tv_sec;
	post_attr.ctime_usec = st.st_ctimespec.tv_nsec / 1000;
	}

	(void)fs_getattr(&dir_fh, &dir_post_attr);

	/* Reply: obj(post_op_fh3) + obj_attr(post_op_attr) + dir_wcc(wcc_data) */
	/* handle_follows=0 so client does LOOKUP fallback for the new file */
	xdr_put_u32(&r->out, NFS_OK);
	xdr_put_u32(&r->out, 0); /* handle_follows = false */
	enc_postop_attr_fattr(&r->out, &post_attr);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &dir_post_attr);
	return PROC_OK;
}

/* REMOVE */
static int
nfs3_remove(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr;
	char name[256];
	char dirpath[512], fullpath[512];

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (unlink(fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	if (fs_getattr(&dir_fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	return PROC_OK;
}

/* RMDIR */
static int
nfs3_rmdir(struct req *r)
{
	struct nfs_fh dir_nfh;
	fhandle_t dir_fh;
	struct fs_fattr pre_attr, post_attr;
	char name[256];
	char dirpath[512], fullpath[512];

	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (rmdir(fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	if (fs_getattr(&dir_fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	return PROC_OK;
}

/* RENAME */
static int
nfs3_rename(struct req *r)
{
	struct nfs_fh from_nfh, to_nfh;
	fhandle_t from_fh, to_fh;
	struct fs_fattr from_pre, from_post, to_pre, to_post;
	char from_name[256], to_name[256];
	char from_path[512], to_path[512], from_full[512], to_full[512];

	if (dec_fh3(&r->in, &from_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, from_name, sizeof(from_name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	from_name[sizeof(from_name)-1] = '\0';

	if (dec_fh3(&r->in, &to_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, to_name, sizeof(to_name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	to_name[sizeof(to_name)-1] = '\0';

	if (fh_lookup_export(&from_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&to_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&from_nfh, &from_fh) < 0 || fs_getattr(&from_fh, &from_pre) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&to_nfh, &to_fh) < 0 || fs_getattr(&to_fh, &to_pre) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&from_nfh)->ro || fh_lookup_export(&to_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	if (nfs3_resolve_dirpath(&from_nfh, from_path, sizeof(from_path)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	if (nfs3_resolve_dirpath(&to_nfh, to_path, sizeof(to_path)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(from_full, sizeof(from_full), "%s/%s", from_path, from_name);
	snprintf(to_full, sizeof(to_full), "%s/%s", to_path, to_name);

	if (rename(from_full, to_full) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	if (fs_getattr(&from_fh, &from_post) != NFS_OK ||
	    fs_getattr(&to_fh, &to_post) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	/* fromdir wcc */
	nfs3_enc_pre_op_attr(&r->out, &from_pre);
	enc_postop_attr_fattr(&r->out, &from_post);
	/* todir wcc */
	nfs3_enc_pre_op_attr(&r->out, &to_pre);
	enc_postop_attr_fattr(&r->out, &to_post);
	return PROC_OK;
}

/* LINK */
static int
nfs3_link(struct req *r)
{
	struct nfs_fh file_nfh, dir_nfh;
	fhandle_t file_fh, dir_fh;
	struct fs_fattr file_attr, pre_attr, post_attr;
	char name[256];
	char dirpath[512], fullpath[512];
	const char *oldpath;

	if (dec_fh3(&r->in, &file_nfh) < 0)
	return PROC_GARBAGE;
	if (dec_fh3(&r->in, &dir_nfh) < 0)
	return PROC_GARBAGE;
	if (xdr_get_string(&r->in, name, sizeof(name) - 1) == 0 ||
	    !xdr_ok(&r->in))
	return PROC_GARBAGE;
	name[sizeof(name)-1] = '\0';

	if (fh_lookup_export(&file_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&file_nfh, &file_fh) < 0 || fs_getattr(&file_fh, &file_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_decode(&dir_nfh, &dir_fh) < 0 || fs_getattr(&dir_fh, &pre_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	if (fh_lookup_export(&dir_nfh)->ro) {
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
	}

	oldpath = fh_path_cache_get(&file_nfh);
	if (oldpath == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	if (nfs3_resolve_dirpath(&dir_nfh, dirpath, sizeof(dirpath)) < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);

	if (link(oldpath, fullpath) < 0) {
	xdr_put_u32(&r->out, nfs_errno(errno));
	return PROC_OK;
	}

	if (fs_getattr(&dir_fh, &post_attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	/* LINK3resok: obj_attr(post_op_attr) + linkdir_wcc(wcc_data) */
	enc_postop_attr_fattr(&r->out, &file_attr);
	nfs3_enc_pre_op_attr(&r->out, &pre_attr);
	enc_postop_attr_fattr(&r->out, &post_attr);
	return PROC_OK;
}

#define P(n, f, d)	{ n, f, d }

static const struct rpc_proc nfs3_procs[] = {
	P("NULL",        nfs3_null,      0),	/*  0 */
	P("GETATTR",     nfs3_getattr,   0),	/*  1 */
	P("SETATTR",     nfs3_setattr,   1),	/*  2 */
	P("LOOKUP",      nfs3_lookup,    0),	/*  3 */
	P("ACCESS",      nfs3_access,    0),	/*  4 */
	P("READLINK",    nfs3_readlink,  0),	/*  5 */
	P("READ",        nfs3_read,      0),	/*  6 */
	P("WRITE",       nfs3_write,     1),	/*  7 */
	P("CREATE",      nfs3_create,    1),	/*  8 */
	P("MKDIR",       nfs3_mkdir,     1),	/*  9 */
	P("SYMLINK",     nfs3_symlink,   1),	/* 10 */
	P("MKNOD",       nfs3_mknod,     1),	/* 11 */
	P("REMOVE",      nfs3_remove,    1),	/* 12 */
	P("RMDIR",       nfs3_rmdir,     1),	/* 13 */
	P("RENAME",      nfs3_rename,    1),	/* 14 */
	P("LINK",        nfs3_link,      1),	/* 15 */
	P("READDIR",     nfs3_readdir,   0),	/* 16 */
	P("READDIRPLUS", nfs3_readdirplus, 0),/* 17 */
	P("FSSTAT",      nfs3_fsstat,    0),	/* 18 */
	P("FSINFO",      nfs3_fsinfo,    0),	/* 19 */
	P("PATHCONF",    nfs3_pathconf,  0),	/* 20 */
	P("COMMIT",      nfs3_commit,    0),	/* 21 */
};

const struct rpc_prog nfs3_prog = {
	RPC_PROG_NFS, 3, sizeof(nfs3_procs) / sizeof(nfs3_procs[0]), nfs3_procs
};
