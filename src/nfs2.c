#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
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
nfs2_setattr(struct req *r)
{
	(void)r;
	(void)xdr_put_u32(&r->out, NFSERR_PERM);
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
	size_t nlen;
	uint32_t nstat;

	if (dec_fh(&r->in, &dir_fh) < 0)
	return PROC_GARBAGE;
	ex = fh_lookup_export(&dir_fh);
	if (ex == NULL) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	return PROC_OK;
	}
	nlen = xdr_get_string(&r->in, name, sizeof(name) - 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	if (nlen == 0 || nlen > NFS2_MAXPATHLEN) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	return PROC_OK;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}

	/* Build full path */
	snprintf(fullpath, sizeof(fullpath), "%s/%s", ex->path, name);

	if (fh_decode(&dir_fh, &dir_kfh) < 0) {
	xdr_put_u32(&r->out, NFSERR_STALE);
	return PROC_OK;
	}

	/* Check parent dir is accessible */
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

	/* Resolve the path */
	if (lgetfh(fullpath, &fh) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	/* Verify same filesystem */
	{
	fhandle_t dir_fh_k;
	if (fh_decode(&dir_fh, &dir_fh_k) < 0) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	if (fh.fh_fsid.val[0] != dir_fh_k.fh_fsid.val[0] ||
	    fh.fh_fsid.val[1] != dir_fh_k.fh_fsid.val[1]) {
	xdr_put_u32(&r->out, NFSERR_NOENT);
	return PROC_OK;
	}
	}

	/* Get attributes of the result */
	nstat = fs_getattr(&fh, &attr);
	if (nstat != NFS_OK) {
xdr_put_u32(&r->out, nstat);
	return PROC_OK;
	}

	/* Encode reply: status, fh, fattr */
	xdr_put_u32(&r->out, NFS_OK);
	fh_encode(&dir_fh, &fh);
	enc_fh(&r->out, &dir_fh);
	nfs2_enc_fattr(&r->out, &attr);
	return PROC_OK;
}

static int
nfs2_readlink(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	char buf[1024];
	ssize_t n;
	uint32_t nstat;
	struct fs_fattr attr;
	int fd;

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

	nstat = fs_getattr(&fh, &attr);
	if (nstat != NFS_OK) {
xdr_put_u32(&r->out, nstat);
	return PROC_OK;
	}
	if (!S_ISLNK(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	return PROC_OK;
	}

	fd = fhopen(&fh, O_RDONLY);
	if (fd < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	return PROC_OK;
	}
	n = readlinkat(fd, ".", buf, sizeof(buf) - 1);
	(void)close(fd);
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
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	nstat = fs_getattr(&fh, &attr);
	if (nstat != NFS_OK) {
xdr_put_u32(&r->out, nstat);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	if (!S_ISREG(attr.mode)) {
	xdr_put_u32(&r->out, NFSERR_INVAL);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}
	/* Check read permission */
	if (fs_access(&attr, r->uid, r->gid, 0400) < 0) {
	xdr_put_u32(&r->out, NFSERR_ACCES);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	fd = fhopen(&fh, O_RDONLY);
	if (fd < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	(void)lseek(fd, offset, SEEK_SET);
	n = read(fd, buf, count);
	(void)close(fd);
	if (n < 0) {
	xdr_put_u32(&r->out, NFSERR_IO);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	xdr_put_u32(&r->out, NFS_OK);
	xdr_put_u32(&r->out, (uint32_t)n);
	xdr_put_var(&r->out, buf, (size_t)n);
	return PROC_OK;
}

static int
nfs2_writecache(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFS_OK);
	return PROC_OK;
}

static int
nfs2_write(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
}

static int
nfs2_create(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_remove(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_rename(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_link(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_symlink(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_mkdir(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_rmdir(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, NFSERR_PERM);
	return PROC_OK;
}

static int
nfs2_readdir(struct req *r)
{
	struct nfs_fh nfh;
	fhandle_t fh;
	uint32_t offset, count;
	DIR *dirp;
	uint64_t inode;
	char name[256];
	size_t mark_pos, used = 0;
	int rc, done = 0;
	struct fs_fattr attr;
	uint32_t nstat;
	int eof_reached = 0;

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

	if (fs_opendir(&fh, &dirp) < 0) {
	log_msg(L_DEBUG, "READDIR: opendir failed");
	xdr_put_u32(&r->out, NFSERR_IO);
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
	}

	/* Skip to offset (cookie) */
	while (offset > 0) {
	rc = fs_readdir(dirp, &inode, name, sizeof(name));
	if (rc != 0)
	break;
	offset--;
	}
	log_msg(L_DEBUG, "READDIR: offset=%u count=%u", (unsigned)offset, (unsigned)count);

	/*
	 * FreeBSD NFSv2 READDIR reply format:
	 *   for each entry:
	 *       more=1(4) + cookie(4) + namelen(4) + name(padded) + next_cookie(4)
	 *   more=0(4)
	 *   eof(4) = 1 if no more data, 0 if more available
	 *
	 * Note: fh_handle is NOT included in the NFSv2 READDIR reply.
	 * The client uses LOOKUP to get file handles for individual entries.
	 */

	while (!done) {
	char fullpath[MAX_PATH_LEN];
	const struct export *ex = fh_lookup_export(&nfh);
	uint32_t this_inode;
	fhandle_t entry_fh_k;

	rc = fs_readdir(dirp, &inode, name, sizeof(name));
	if (rc != 0) {
	done = 1;
	eof_reached = 1;
	continue;
	}
	/* Include . and .. - the client needs them for cookie advancement */

	/* Get handle for this entry (for validation, not sent in reply) */
	snprintf(fullpath, sizeof(fullpath), "%s/%s", ex->path, name);
	if (lgetfh(fullpath, &entry_fh_k) < 0)
	continue;

	this_inode = (uint32_t)inode;

	/* Check if entry fits within count */
	{
	size_t entry_overhead = 4 + 4 + XDR_PAD(strlen(name)) + 4;
	if (xdr_pos(&r->out) + entry_overhead > count + 24)
	done = 1;
	else {
	/* more=1 */
	xdr_put_u32(&r->out, 1);
	/* cookie */
	xdr_put_u32(&r->out, this_inode);
	/* namelen + name (padded) */
	xdr_put_string(&r->out, name);
	/* next_cookie */
	xdr_put_u32(&r->out, this_inode);
	}
	}
	}

	(void)closedir(dirp);

	/* more=0 */
	xdr_put_u32(&r->out, 0);

	/* eof flag: 1 = end of directory, 0 = more available */
	if (eof_reached)
	xdr_put_u32(&r->out, 1);  /* EOF */
	else
	xdr_put_u32(&r->out, 0);  /* more available */

	{
	size_t pos = xdr_pos(&r->out);
	uint8_t *data = r->out.base;
	size_t i;
	log_msg(L_DEBUG, "READDIR: reply %zu bytes:", pos);
	for (i = 0; i < pos && i < 128; i += 4) {
	log_msg(L_DEBUG, "  %04zx: %02x%02x%02x%02x", i,
	    data[i], data[i+1], data[i+2], data[i+3]);
	}
	}
	log_msg(L_DEBUG, "READDIR: done=%d eof=%d pos=%zu", done, eof_reached, xdr_pos(&r->out));
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
