#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <time.h>

#include "conf.h"
#include "drc.h"
#include "log.h"
#include "rpc.h"

#define RPC_VERSION	2u
#define MSG_CALL	0u
#define MSG_REPLY	1u
#define REPLY_ACCEPTED	0u
#define REPLY_DENIED	1u

#define AS_SUCCESS	0u
#define AS_PROG_UNAVAIL	1u
#define AS_PROG_MISMATCH	2u
#define AS_PROC_UNAVAIL	3u
#define AS_GARBAGE_ARGS	4u

#define RJ_RPC_MISMATCH	0u
#define RJ_AUTH_ERROR	1u

#define AUTHF_NONE	0u
#define AUTHF_SYS	1u
#define AUTHERR_BADCRED	1u

#define RPC_MAXAUTH	400u
#define RPC_MAXPROGS	16

static const struct rpc_prog *progs[RPC_MAXPROGS];
static unsigned nprogs;

int
rpc_register(const struct rpc_prog *p)
{
	if (nprogs >= RPC_MAXPROGS)
	return -1;
	progs[nprogs++] = p;
	return 0;
}

static void
put_accepted(struct xdr *o, uint32_t xid, uint32_t stat)
{
	xdr_put_u32(o, xid);
	xdr_put_u32(o, MSG_REPLY);
	xdr_put_u32(o, REPLY_ACCEPTED);
	xdr_put_u32(o, AUTHF_NONE);	/* verifier flavor */
	xdr_put_u32(o, 0);	/* verifier length */
	xdr_put_u32(o, stat);
}

static void
put_rpc_mismatch(struct xdr *o, uint32_t xid)
{
	xdr_put_u32(o, xid);
	xdr_put_u32(o, MSG_REPLY);
	xdr_put_u32(o, REPLY_DENIED);
	xdr_put_u32(o, RJ_RPC_MISMATCH);
	xdr_put_u32(o, RPC_VERSION);
	xdr_put_u32(o, RPC_VERSION);
}

static void
put_auth_error(struct xdr *o, uint32_t xid, uint32_t stat)
{
	xdr_put_u32(o, xid);
	xdr_put_u32(o, MSG_REPLY);
	xdr_put_u32(o, REPLY_DENIED);
	xdr_put_u32(o, RJ_AUTH_ERROR);
	xdr_put_u32(o, stat);
}

static size_t
finish(struct req *r)
{
	if (!xdr_ok(&r->out)) {
	log_msg(L_ERR, "reply buffer overflow, xid=%08lx",
	    (unsigned long)r->xid);
	return 0;
	}
	return xdr_pos(&r->out);
}

/* Parse the credential body. Return 0 on success, or an auth_stat. */
static uint32_t
parse_cred(struct req *r, uint32_t flavor, unsigned char *body, size_t n)
{
	struct xdr c;
	uint32_t cnt, g, i;

	r->have_cred = 0;
	if (flavor == AUTHF_NONE)
	return 0;
	if (flavor != AUTHF_SYS)
	return AUTHERR_BADCRED;

	xdr_init(&c, body, n);
	(void)xdr_get_u32(&c);	/* stamp */
	xdr_skip_var(&c, 255);	/* machine name */
	r->uid = xdr_get_u32(&c);
	r->gid = xdr_get_u32(&c);
	cnt = xdr_get_u32(&c);
	r->ngids = 0;
	/* The loop ends at the first overrun, so a huge cnt is safe. */
	for (i = 0; i < cnt && xdr_ok(&c); i++) {
	g = xdr_get_u32(&c);
	if (r->ngids < RPC_MAXGIDS)
	r->gids[r->ngids++] = g;
	}
	if (!xdr_ok(&c))
	return AUTHERR_BADCRED;
	r->have_cred = 1;
	return 0;
}

size_t
rpc_handle(struct req *r, void *buf, size_t len, void *reply,
    size_t replymax)
{
	uint32_t msgtype, rpcvers, cflavor, vflavor, authstat;
	uint32_t lo, hi;
	unsigned char cbody[RPC_MAXAUTH];
	size_t cn;
	const struct rpc_prog *p;
	unsigned i;
	int found, rc, use_drc = 0;
	struct drc_key dk;
	size_t dn;

	xdr_init(&r->in, buf, len);
	xdr_init(&r->out, reply, replymax);
	r->have_cred = 0;
	r->uid = r->gid = 0;
	r->ngids = 0;

	r->xid = xdr_get_u32(&r->in);
	msgtype = xdr_get_u32(&r->in);
	rpcvers = xdr_get_u32(&r->in);
	r->prog = xdr_get_u32(&r->in);
	r->vers = xdr_get_u32(&r->in);
	r->proc = xdr_get_u32(&r->in);
	cflavor = xdr_get_u32(&r->in);
	cn = xdr_get_var(&r->in, cbody, sizeof(cbody));
	vflavor = xdr_get_u32(&r->in);
	xdr_skip_var(&r->in, RPC_MAXAUTH);
	(void)vflavor;

	/* Truncated or damaged header: drop it. */
	if (!xdr_ok(&r->in) || msgtype != MSG_CALL) {
	log_msg(L_DEBUG, "bad RPC header from %s:%u, dropped",
	    inet_ntoa(r->peer.sin_addr), ntohs(r->peer.sin_port));
	return 0;
	}

	if (rpcvers != RPC_VERSION) {
	put_rpc_mismatch(&r->out, r->xid);
	return finish(r);
	}

	authstat = parse_cred(r, cflavor, cbody, cn);
	if (authstat != 0) {
	put_auth_error(&r->out, r->xid, authstat);
	return finish(r);
	}

	/* Find the program and version. */
	found = 0;
	lo = 0xFFFFFFFFu;
	hi = 0;
	p = NULL;
	for (i = 0; i < nprogs; i++) {
	if (progs[i]->prog != r->prog)
	continue;
	found = 1;
	if (progs[i]->vers < lo)
	lo = progs[i]->vers;
	if (progs[i]->vers > hi)
	hi = progs[i]->vers;
	if (progs[i]->vers == r->vers)
	p = progs[i];
	}

	log_msg(L_DEBUG, "call prog=%lu v%lu proc=%lu(%s) xid=%08lx "
	    "from %s:%u uid=%lu",
	    (unsigned long)r->prog, (unsigned long)r->vers,
	    (unsigned long)r->proc,
	    (p != NULL && r->proc < p->nprocs) ? p->procs[r->proc].name : "?",
	    (unsigned long)r->xid,
	    inet_ntoa(r->peer.sin_addr), ntohs(r->peer.sin_port),
	    (unsigned long)r->uid);

	if (!found) {
	put_accepted(&r->out, r->xid, AS_PROG_UNAVAIL);
	return finish(r);
	}
	if (p == NULL) {
	put_accepted(&r->out, r->xid, AS_PROG_MISMATCH);
	xdr_put_u32(&r->out, lo);
	xdr_put_u32(&r->out, hi);
	return finish(r);
	}
	if (r->proc >= p->nprocs) {
	put_accepted(&r->out, r->xid, AS_PROC_UNAVAIL);
	return finish(r);
	}

	conf_set_client(r->peer.sin_addr);

	/*
	 * Non-idempotent call over UDP: if we already answered this exact
	 * call, send the same reply and do not run the procedure again.
	 */
	if (p->procs[r->proc].drc && !r->is_tcp) {
	dk.addr = r->peer.sin_addr;
	dk.port = r->peer.sin_port;
	dk.xid = r->xid;
	dk.prog = r->prog;
	dk.vers = r->vers;
	dk.proc = r->proc;
	dk.chk = drc_checksum(buf, len);
	use_drc = 1;
	if (drc_lookup(&dk, time(NULL), reply, replymax, &dn)) {
	log_msg(L_DEBUG, "duplicate call xid=%08lx: reply repeated",
	    (unsigned long)r->xid);
	return dn;
	}
	}

	put_accepted(&r->out, r->xid, AS_SUCCESS);
	rc = p->procs[r->proc].fn(r);

	if (rc == PROC_OK) {
	size_t n = finish(r);

	if (use_drc && n > 0)
	drc_store(&dk, time(NULL), reply, n);
	return n;
	}
	if (rc == PROC_GARBAGE || rc == PROC_UNAVAIL) {
	xdr_init(&r->out, reply, replymax);
	put_accepted(&r->out, r->xid,
	    rc == PROC_GARBAGE ? AS_GARBAGE_ARGS : AS_PROC_UNAVAIL);
	return finish(r);
	}
	return 0;	/* PROC_DROP */
}
