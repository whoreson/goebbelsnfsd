#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "fh.h"
#include "fs.h"
#include "log.h"
#include "port.h"
#include "progs.h"
#include "rpc.h"

#define MNT_PATH_MAX	1024
#define MNT_NAM_MAX	255

/* MOUNT v3 status codes */
#define MNT3_OK	0
#define MNT3ERR_PERM	1
#define MNT3ERR_NOENT	2
#define MNT3ERR_IO	5
#define MNT3ERR_ACCES	13
#define MNT3ERR_NOTDIR	20
#define MNT3ERR_INVAL	22
#define MNT3ERR_NAMETOOLONG	63

static int mnt1_null(struct req *r);
static int mnt1_mnt(struct req *r);
static int mnt1_dump(struct req *r);
static int mnt1_umnt(struct req *r);
static int mnt1_umntall(struct req *r);
static int mnt1_export(struct req *r);

static const struct rpc_proc mount1_procs[] = {
	{ "NULL",    mnt1_null,    0 },
	{ "MNT",     mnt1_mnt,     0 },
	{ "DUMP",    mnt1_dump,    0 },
	{ "UMNT",    mnt1_umnt,    0 },
	{ "UMNTALL", mnt1_umntall, 0 },
	{ "EXPORT",  mnt1_export,  0 }
};

static int mnt3_null(struct req *r);
static int mnt3_mnt(struct req *r);
static int mnt3_dump(struct req *r);
static int mnt3_umnt(struct req *r);
static int mnt3_umntall(struct req *r);
static int mnt3_export(struct req *r);

static const struct rpc_proc mount3_procs[] = {
	{ "NULL",    mnt3_null,    0 },
	{ "MNT",     mnt3_mnt,     0 },
	{ "DUMP",    mnt3_dump,    0 },
	{ "UMNT",    mnt3_umnt,    0 },
	{ "UMNTALL", mnt3_umntall, 0 },
	{ "EXPORT",  mnt3_export,  0 }
};

const struct rpc_prog mount1_prog = { RPC_PROG_MOUNT, 1,
    sizeof(mount1_procs) / sizeof(mount1_procs[0]), mount1_procs };
const struct rpc_prog mount3_prog = { RPC_PROG_MOUNT, 3,
    sizeof(mount3_procs) / sizeof(mount3_procs[0]), mount3_procs };

static void
enc_fh(struct xdr *x, const struct nfs_fh *nfh)
{
	xdr_put_var(x, nfh, NFS_FH_SIZE);
}

static int
mnt1_null(struct req *r)
{
	(void)r;
	return PROC_OK;
}

static int
mnt3_null(struct req *r)
{
	(void)r;
	return PROC_OK;
}

/*
 * MOUNT v1 MNT -> fhstatus.
 * fhstatus = union switch(u_int fhs_status) {
 *   case 0: opaque fhs_fhandle[32]; default: void; }
 */
static int
mnt1_mnt(struct req *r)
{
	const struct export *ex;
	fhandle_t fh;
	struct nfs_fh nfh;
	char path[MNT_PATH_MAX];
	int nstat;

	xdr_get_string(&r->in, path, sizeof(path) - 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (strstr(path, "..") != NULL) {
	xdr_put_u32(&r->out, 1);
	return PROC_OK;
	}

	ex = conf_lookup(path, r->peer.sin_addr);
	if (ex == NULL) {
	xdr_put_u32(&r->out, 2);
	return PROC_OK;
	}

	if (port_lgetfh(path, &fh) < 0) {
	xdr_put_u32(&r->out, 5);
	return PROC_OK;
	}

#ifdef __FreeBSD__
	if (fh.fh_fsid.val[0] != ex->fsid_val[0] ||
	    fh.fh_fsid.val[1] != ex->fsid_val[1]) {
	xdr_put_u32(&r->out, 2);
	return PROC_OK;
	}
#endif

	nstat = 0;
	xdr_put_u32(&r->out, nstat);
	fh_encode(&nfh, &fh);
	/* MOUNT v1: fhandle is opaque[32] (fixed size, no length prefix) */
	xdr_put_fixed(&r->out, &nfh, NFS_FH_SIZE);
	log_msg(L_DEBUG, "MNT1 %s -> ok", path);
	return PROC_OK;
}

/*
 * MOUNT v3 MNT -> mountres3.
 * mountres3 = union switch(mountstat3 fhs_status) {
 *   case MNT3_OK: { fhandle3 fhandle; int auth_flavors<>; }; default: void; }
 */
static int
mnt3_mnt(struct req *r)
{
	const struct export *ex;
	fhandle_t fh;
	struct nfs_fh nfh;
	char path[MNT_PATH_MAX];

	xdr_get_string(&r->in, path, sizeof(path) - 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;

	if (strstr(path, "..") != NULL) {
	xdr_put_u32(&r->out, MNT3ERR_PERM);
	return PROC_OK;
	}

	ex = conf_lookup(path, r->peer.sin_addr);
	if (ex == NULL) {
	xdr_put_u32(&r->out, MNT3ERR_NOENT);
	return PROC_OK;
	}

	if (port_lgetfh(path, &fh) < 0) {
	xdr_put_u32(&r->out, MNT3ERR_IO);
	return PROC_OK;
	}

#ifdef __FreeBSD__
	if (fh.fh_fsid.val[0] != ex->fsid_val[0] ||
	    fh.fh_fsid.val[1] != ex->fsid_val[1]) {
	xdr_put_u32(&r->out, MNT3ERR_NOENT);
	return PROC_OK;
	}
#endif

	/* MNT3_OK */
	xdr_put_u32(&r->out, MNT3_OK);
	fh_encode(&nfh, &fh);
	/* Cache path for READLINK */
	fh_path_cache_add(&nfh, path);
	/* fhandle3: var-length opaque, max 64 */
	xdr_put_u32(&r->out, NFS_FH_SIZE);
	xdr_put_fixed(&r->out, &nfh, NFS_FH_SIZE);
	/* auth_flavors: empty array */
	xdr_put_u32(&r->out, 0);
	log_msg(L_DEBUG, "MNT3 %s -> ok", path);
	return PROC_OK;
}

/*
 * MOUNT DUMP -> mountlist.
 * mountlist = struct mountbody *
 * struct mountbody { name ml_hostname; dirpath ml_directory; mountlist ml_next; }
 * NULL pointer (u32=0) terminates the list.
 * No mount tracking yet, so return empty list.
 */
static int
mnt1_dump(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
}

static int
mnt3_dump(struct req *r)
{
	(void)r;
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
}

static int
mnt1_umnt(struct req *r)
{
	char path[MNT_PATH_MAX];

	xdr_get_string(&r->in, path, sizeof(path) - 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	return PROC_OK;
}

static int
mnt3_umnt(struct req *r)
{
	char path[MNT_PATH_MAX];

	xdr_get_string(&r->in, path, sizeof(path) - 1);
	if (!xdr_ok(&r->in))
	return PROC_GARBAGE;
	return PROC_OK;
}

static int
mnt1_umntall(struct req *r)
{
	(void)r;
	return PROC_OK;
}

static int
mnt3_umntall(struct req *r)
{
	(void)r;
	return PROC_OK;
}

/* MOUNT EXPORT returns linked list of exportnode structs.
 * Each node has: dirpath (string), groups (linked list of groupnode),
 * and next pointer. Each groupnode has: name (string) and next pointer.
 * XDR pointers are u32: 0=NULL, 1=non-NULL (struct follows). */
static int
mnt1_export(struct req *r)
{
	unsigned i;

	/* Iterate exports, building a linked list */
	for (i = 0; i < conf_export_count(); i++) {
	const struct export *ex = conf_get_export(i);
	char ipstr[16];

	/* exports pointer: non-NULL */
	xdr_put_u32(&r->out, 1);

	/* exportnode.ex_dir */
	xdr_put_string(&r->out, ex->path);

	/* exportnode.ex_groups: non-NULL (one group) */
	xdr_put_u32(&r->out, 1);

	if (ex->net.s_addr != htonl(0)) {
	snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
	    (unsigned)(ex->net.s_addr >> 24) & 0xFF,
	    (unsigned)(ex->net.s_addr >> 16) & 0xFF,
	    (unsigned)(ex->net.s_addr >> 8) & 0xFF,
	    (unsigned)ex->net.s_addr & 0xFF);
	xdr_put_string(&r->out, ipstr);
	} else {
	xdr_put_string(&r->out, "*");
	}

	/* groupnode.gr_next: NULL (no more groups) */
	xdr_put_u32(&r->out, 0);
	}
	/* exports list terminator: NULL pointer */
	xdr_put_u32(&r->out, 0);
	return PROC_OK;
}

static int
mnt3_export(struct req *r)
{
	return mnt1_export(r);
}
