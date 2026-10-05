#include "progs.h"
#include "rpc.h"

static int nfs3_null(struct req *r) { (void)r; return PROC_OK; }
static int nfs3_notimpl(struct req *r) { (void)r; return PROC_UNAVAIL; }

#define P(n, f, d)	{ n, f, d }

static const struct rpc_proc nfs3_procs[] = {
	P("NULL",        nfs3_null,    0),	/*  0 */
	P("GETATTR",     nfs3_notimpl, 0),	/*  1 */
	P("SETATTR",     nfs3_notimpl, 1),	/*  2 */
	P("LOOKUP",      nfs3_notimpl, 0),	/*  3 */
	P("ACCESS",      nfs3_notimpl, 0),	/*  4 */
	P("READLINK",    nfs3_notimpl, 0),	/*  5 */
	P("READ",        nfs3_notimpl, 0),	/*  6 */
	P("WRITE",       nfs3_notimpl, 1),	/*  7 */
	P("CREATE",      nfs3_notimpl, 1),	/*  8 */
	P("MKDIR",       nfs3_notimpl, 1),	/*  9 */
	P("SYMLINK",     nfs3_notimpl, 1),	/* 10 */
	P("MKNOD",       nfs3_notimpl, 1),	/* 11 */
	P("REMOVE",      nfs3_notimpl, 1),	/* 12 */
	P("RMDIR",       nfs3_notimpl, 1),	/* 13 */
	P("RENAME",      nfs3_notimpl, 1),	/* 14 */
	P("LINK",        nfs3_notimpl, 1),	/* 15 */
	P("READDIR",     nfs3_notimpl, 0),	/* 16 */
	P("READDIRPLUS", nfs3_notimpl, 0),	/* 17 */
	P("FSSTAT",      nfs3_notimpl, 0),	/* 18 */
	P("FSINFO",      nfs3_notimpl, 0),	/* 19 */
	P("PATHCONF",    nfs3_notimpl, 0),	/* 20 */
	P("COMMIT",      nfs3_notimpl, 0)	/* 21 */
};

const struct rpc_prog nfs3_prog = {
	RPC_PROG_NFS, 3, sizeof(nfs3_procs) / sizeof(nfs3_procs[0]), nfs3_procs
};
