#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include "drc.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

static struct drc_key
key(uint32_t xid, const char *ip, uint16_t port, uint32_t chk)
{
	struct drc_key k;

	memset(&k, 0, sizeof(k));
	inet_pton(AF_INET, ip, &k.addr);
	k.port = port;
	k.xid = xid;
	k.prog = 100003;
	k.vers = 3;
	k.proc = 12;
	k.chk = chk;
	return k;
}

int
main(void)
{
	struct drc_key a = key(1, "10.0.0.1", 700, 55);
	char out[DRC_MAXREPLY];
	char big[DRC_MAXREPLY + 1];
	size_t n;
	time_t t = 1000;
	unsigned i;

	drc_clear();
	CHECK(!drc_lookup(&a, t, out, sizeof(out), &n));
	drc_store(&a, t, "REPLY", 5);
	CHECK(drc_lookup(&a, t + 1, out, sizeof(out), &n));
	CHECK(n == 5 && memcmp(out, "REPLY", 5) == 0);

	/* A different client, port, xid or call body is a different call. */
	{
		struct drc_key b = key(1, "10.0.0.2", 700, 55);
		struct drc_key c = key(1, "10.0.0.1", 701, 55);
		struct drc_key d = key(2, "10.0.0.1", 700, 55);
		struct drc_key e = key(1, "10.0.0.1", 700, 56);

		CHECK(!drc_lookup(&b, t, out, sizeof(out), &n));
		CHECK(!drc_lookup(&c, t, out, sizeof(out), &n));
		CHECK(!drc_lookup(&d, t, out, sizeof(out), &n));
		CHECK(!drc_lookup(&e, t, out, sizeof(out), &n));
	}

	/* Entries expire. */
	CHECK(!drc_lookup(&a, t + DRC_TTL + 1, out, sizeof(out), &n));

	/* Too large replies are not kept. */
	{
		struct drc_key k = key(9, "10.0.0.1", 700, 1);

		memset(big, 'x', sizeof(big));
		drc_store(&k, t, big, sizeof(big));
		CHECK(!drc_lookup(&k, t, out, sizeof(out), &n));
	}

	/* The oldest entry is replaced when the table is full. */
	drc_clear();
	drc_store(&a, t, "first", 5);
	for (i = 0; i < DRC_SLOTS; i++) {
		struct drc_key k = key(100 + i, "10.0.0.9", 800, 7);

		drc_store(&k, t, "x", 1);
	}
	CHECK(!drc_lookup(&a, t, out, sizeof(out), &n));
	{
		struct drc_key k = key(100 + DRC_SLOTS - 1, "10.0.0.9", 800, 7);

		CHECK(drc_lookup(&k, t, out, sizeof(out), &n));
	}

	/* Checksum differs for different messages. */
	CHECK(drc_checksum("abc", 3) != drc_checksum("abd", 3));
	CHECK(drc_checksum("abc", 3) == drc_checksum("abc", 3));

	if (fails == 0)
		printf("drc_test: OK\n");
	return fails != 0;
}
