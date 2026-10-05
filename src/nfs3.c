#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "progs.h"
#include "rpc.h"
#include "log.h"
#include "xdr.h"
#include "fh.h"
#include "fs.h"
#include "conf.h"
#include "nfs_common.h"

#define NFS3_MAXDATA	32768
#define NFS3_MAXRDIR	32768

static int nfs3_null(struct req *r) { (void)r; return PROC_OK; }
static int nfs3_notimpl(struct req *r) { (void)r; return PROC_UNAVAIL; }

/* NFSv3 file handle: opaque <NFSX_V3FHMAX> (len prefix + data) */
static void
enc_fh3(struct xdr *x, struct nfs_fh *nfh)
{
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
static void
enc_postop_attr(struct xdr *x, const struct nfs_fh *nfh)
{
	if (nfh == NULL) {
	xdr_put_u32(x, 0);
	return;
	}
	fhandle_t fh;
	struct fs_fattr attr;
	if (fh_decode(nfh, &fh) >= 0 && fs_getattr(&fh, &attr) == NFS_OK) {
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
	struct fs_fattr attr;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	/* Skip guard (sa_call_since) */
	xdr_skip_var(&r->in, 256);
	/* Skip sattr3 (60 bytes max) */
	xdr_skip_var(&r->in, 64);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	/* Get attributes */
	if (fh_decode(&nfh, &fh) < 0 || fs_getattr(&fh, &attr) != NFS_OK) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	/* WCC data */
	nfs3_enc_wcc_attr(&r->out, &attr);
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
}

/* LOOKUP */
static int
nfs3_lookup(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh, fh_child;
	const struct export *ex;
	struct nfs_fh child_nfh;
	char name[256];
	uint32_t nstat;
	char fullpath[512];

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

	ex = fh_lookup_export(&nfh);
	snprintf(fullpath, sizeof(fullpath), "%s/%s", ex->path, name);
	if (lgetfh(fullpath, &fh_child) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	enc_postop_attr(&r->out, &nfh);
	return PROC_OK;
	}
	fh_encode(&child_nfh, &fh_child);

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
	if (req_access & 1 && (attr.mode & 0400))
	granted |= 1;
	if (req_access & 2 && S_ISDIR(attr.mode) && (attr.mode & 0100))
	granted |= 2;
	/* MODIFY, ADD, DELETE - read-only server, never granted */
	if (req_access & 32 && (attr.mode & 0100))
	granted |= 32;

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
	if (!S_ISLNK(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);  /* data length = 0 */
	return PROC_OK;
	}
	n = fs_readlink(&fh, link, sizeof(link));
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
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
	size_t mark;

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

	/* reply: status + postop_attr + count + data_len + data */
	xdr_put_u32(&r->out, NFS_OK);
	enc_postop_attr(&r->out, &nfh);
	xdr_put_u32(&r->out, 0);  /* count, patch later */
	mark = xdr_pos(&r->out);
	xdr_put_u32(&r->out, 0);  /* data_len, patch later */
	n = read(fd, r->out.base + xdr_pos(&r->out), count);
	if (n < 0)
	n = 0;
	(void)close(fd);

	/* Patch count and data_len */
	{ size_t pos = xdr_pos(&r->out);
	size_t datalen = (size_t)n;
	/* count */
	xdr_patch_u32(&r->out, mark - 4, (uint32_t)datalen);
	/* data_len */
	xdr_patch_u32(&r->out, mark, (uint32_t)datalen);
	/* advance past data */
	r->out.pos += datalen;
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
	size_t entry_size = 8 + 4 + XDR_PAD(namelen) + 8 + 4 + 4;
	if (xdr_pos(&r->out) + entry_size > count + 24)
	done = 1;
	else {
	xdr_put_u64(&r->out, inode);             /* fileid */
	xdr_put_string(&r->out, name);            /* name */
	xdr_put_u64(&r->out, cur);                /* cookie */
	xdr_put_u32(&r->out, 0);                  /* name_attributes: attr_follows=0 */
	xdr_put_u32(&r->out, done ? 0 : 1);       /* nextentry */
	}
	}
	cur++;
	}
	}

	/* NULL pointer to terminate entry list */
	xdr_put_u32(&r->out, 0);

	/* eof */
	if (done)
	xdr_put_u32(&r->out, 1);
	else
	xdr_put_u32(&r->out, 0);

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
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u32(&r->out, NFS3_MAXDATA);
	xdr_put_u64(&r->out, 1ULL << 32);
	xdr_put_u32(&r->out, 1);
	xdr_put_u32(&r->out, 0);
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

/* COMMIT */
static int
nfs3_commit(struct req *r)
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
	xdr_put_u32(&r->out, 0);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
}

/* Write operations - all return NFSERR_ROFS */
static int
nfs3_ro_op(struct req *r)
{
	struct nfs_fh nfh;

	if (dec_fh3(&r->in, &nfh) < 0)
	return PROC_GARBAGE;
	if (fh_lookup_export(&nfh) == NULL) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}
	xdr_put_u32(&r->out, NFSERR_ROFS);
	return PROC_OK;
}

static int nfs3_write(struct req *r)    { return nfs3_ro_op(r); }
static int nfs3_create(struct req *r)   { return nfs3_ro_op(r); }
static int nfs3_mkdir(struct req *r)    { return nfs3_ro_op(r); }
static int nfs3_symlink(struct req *r)  { return nfs3_ro_op(r); }
static int nfs3_mknod(struct req *r)    { return nfs3_ro_op(r); }
static int nfs3_remove(struct req *r)   { return nfs3_ro_op(r); }
static int nfs3_rmdir(struct req *r)    { return nfs3_ro_op(r); }
static int nfs3_rename(struct req *r)   { return nfs3_ro_op(r); }
static int nfs3_link(struct req *r)     { return nfs3_ro_op(r); }

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
