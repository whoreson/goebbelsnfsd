#include "progs.h"
#include "rpc.h"

static int mnt_null(struct req *r) { (void)r; return PROC_OK; }
static int mnt_notimpl(struct req *r) { (void)r; return PROC_UNAVAIL; }

/* MOUNT v1 and v3 use the same procedure numbers 0 to 5. */
static const struct rpc_proc mount_procs[] = {
	{ "NULL",    mnt_null,    0 },	/* 0 */
	{ "MNT",     mnt_notimpl, 0 },	/* 1 */
	{ "DUMP",    mnt_notimpl, 0 },	/* 2 */
	{ "UMNT",    mnt_notimpl, 0 },	/* 3 */
	{ "UMNTALL", mnt_notimpl, 0 },	/* 4 */
	{ "EXPORT",  mnt_notimpl, 0 }	/* 5 */
};

#define NMOUNT	(sizeof(mount_procs) / sizeof(mount_procs[0]))

const struct rpc_prog mount1_prog = { RPC_PROG_MOUNT, 1, NMOUNT, mount_procs };
const struct rpc_prog mount3_prog = { RPC_PROG_MOUNT, 3, NMOUNT, mount_procs };
