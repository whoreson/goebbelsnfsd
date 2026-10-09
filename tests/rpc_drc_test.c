/* A repeated UDP call must not run a non-idempotent procedure twice. */
#include "port.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include "rpc.h"

static int fails, runs_drc, runs_plain;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

static int
proc_drc(struct req *r)
{
	runs_drc++;
	xdr_put_u32(&r->out, (uint32_t)runs_drc);
	return PROC_OK;
}

static int
proc_plain(struct req *r)
{
	runs_plain++;
	xdr_put_u32(&r->out, (uint32_t)runs_plain);
	return PROC_OK;
}

static const struct rpc_proc procs[] = {
	{ "PLAIN", proc_plain, 0 },
	{ "DRC", proc_drc, 1 },
};
static const struct rpc_prog prog = { 200000, 1, 2, procs };

static size_t
call(uint32_t xid, uint32_t proc, int tcp, unsigned char *reply)
{
	unsigned char msg[64];
	struct xdr x;
	struct req r;

	xdr_init(&x, msg, sizeof(msg));
	xdr_put_u32(&x, xid);
	xdr_put_u32(&x, 0);		/* CALL */
	xdr_put_u32(&x, 2);		/* rpc version */
	xdr_put_u32(&x, 200000);
	xdr_put_u32(&x, 1);
	xdr_put_u32(&x, proc);
	xdr_put_u32(&x, 0); xdr_put_u32(&x, 0);	/* cred AUTH_NONE */
	xdr_put_u32(&x, 0); xdr_put_u32(&x, 0);	/* verf AUTH_NONE */
	memset(&r, 0, sizeof(r));
	r.is_tcp = tcp;
	r.peer.sin_family = AF_INET;
	r.peer.sin_port = htons(700);
	inet_pton(AF_INET, "10.0.0.1", &r.peer.sin_addr);
	return rpc_handle(&r, msg, xdr_pos(&x), reply, 256);
}

int
main(void)
{
	unsigned char r1[256], r2[256], r3[256];
	size_t n1, n2, n3;

	CHECK(rpc_register(&prog) == 0);

	n1 = call(7, 1, 0, r1);
	n2 = call(7, 1, 0, r2);		/* same xid: retransmission */
	CHECK(n1 > 0 && n1 == n2 && memcmp(r1, r2, n1) == 0);
	CHECK(runs_drc == 1);

	n3 = call(8, 1, 0, r3);		/* new xid: a new call */
	CHECK(n3 > 0 && runs_drc == 2);

	/* TCP does not retransmit: no cache, the call runs again. */
	(void)call(9, 1, 1, r1);
	(void)call(9, 1, 1, r2);
	CHECK(runs_drc == 4);

	/* Idempotent procedures are never cached. */
	(void)call(10, 0, 0, r1);
	(void)call(10, 0, 0, r2);
	CHECK(runs_plain == 2);

	if (fails == 0)
		printf("rpc_drc_test: OK\n");
	return fails != 0;
}
