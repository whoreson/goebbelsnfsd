#ifndef RPC_H
#define RPC_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>

#include "xdr.h"

#define RPC_MAXGIDS	16

/* Return codes from a procedure handler. */
#define PROC_OK	0	/* reply body is in r->out */
#define PROC_GARBAGE	1	/* arguments did not decode: GARBAGE_ARGS */
#define PROC_DROP	2	/* send no reply */
#define PROC_UNAVAIL	3	/* procedure not implemented: PROC_UNAVAIL */

struct req {
	/* transport */
	int                fd;
	int                is_tcp;
	struct sockaddr_in peer;
	struct in_addr     local;	/* destination address of the request */

	/* RPC call header */
	uint32_t           xid;
	uint32_t           prog, vers, proc;

	/* credentials (AUTH_SYS) */
	int                have_cred;	/* 0 for AUTH_NONE */
	uint32_t           uid, gid;
	uint32_t           ngids;
	uint32_t           gids[RPC_MAXGIDS];

	/* message bodies. "in" starts after the call header.
	 * "out" starts after the accepted-reply header. */
	struct xdr         in;
	struct xdr         out;
};

typedef int (*proc_fn)(struct req *r);

struct rpc_proc {
	const char *name;
	proc_fn     fn;
	int         drc;	/* 1: not idempotent. Use the duplicate cache. */
};

struct rpc_prog {
	uint32_t               prog, vers;
	unsigned               nprocs;
	const struct rpc_proc *procs;
};

#define RPC_PROG_NFS	100003u
#define RPC_PROG_MOUNT	100005u

/* Register a program/version. Call at startup. */
int rpc_register(const struct rpc_prog *p);

/* Parse one call message in "buf" and dispatch it.
 * Build the full reply (header + body) in "reply".
 * Return reply length, or 0 for no reply. */
size_t rpc_handle(struct req *r, void *buf, size_t len,
    void *reply, size_t replymax);

#endif
