#include "progs.h"
#include "rpc.h"

static int nfs2_null(struct req *r) { (void)r; return PROC_OK; }
static int nfs2_notimpl(struct req *r) { (void)r; return PROC_UNAVAIL; }

static const struct rpc_proc nfs2_procs[] = {
	{ "NULL",       nfs2_null,    0 },	/*  0 */
	{ "GETATTR",    nfs2_notimpl, 0 },	/*  1 */
	{ "SETATTR",    nfs2_notimpl, 1 },	/*  2 */
	{ "ROOT",       nfs2_notimpl, 0 },	/*  3 obsolete */
	{ "LOOKUP",     nfs2_notimpl, 0 },	/*  4 */
	{ "READLINK",   nfs2_notimpl, 0 },	/*  5 */
	{ "READ",       nfs2_notimpl, 0 },	/*  6 */
	{ "WRITECACHE", nfs2_notimpl, 0 },	/*  7 obsolete */
	{ "WRITE",      nfs2_notimpl, 1 },	/*  8 */
	{ "CREATE",     nfs2_notimpl, 1 },	/*  9 */
	{ "REMOVE",     nfs2_notimpl, 1 },	/* 10 */
	{ "RENAME",     nfs2_notimpl, 1 },	/* 11 */
	{ "LINK",       nfs2_notimpl, 1 },	/* 12 */
	{ "SYMLINK",    nfs2_notimpl, 1 },	/* 13 */
	{ "MKDIR",      nfs2_notimpl, 1 },	/* 14 */
	{ "RMDIR",      nfs2_notimpl, 1 },	/* 15 */
	{ "READDIR",    nfs2_notimpl, 0 },	/* 16 */
	{ "STATFS",     nfs2_notimpl, 0 }	/* 17 */
};

const struct rpc_prog nfs2_prog = {
	RPC_PROG_NFS, 2, sizeof(nfs2_procs) / sizeof(nfs2_procs[0]), nfs2_procs
};
