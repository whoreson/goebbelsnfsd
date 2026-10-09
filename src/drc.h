#ifndef DRC_H
#define DRC_H

/*
 * Duplicate request cache.
 *
 * A UDP client sends a call again if the reply is lost. Running a
 * non-idempotent call twice gives a wrong result: the second REMOVE
 * returns NOENT, the second CREATE (exclusive) returns EXIST.
 * The server keeps the reply of each such call for a short time and
 * sends the same reply again for a repeated call.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <netinet/in.h>

#define DRC_SLOTS	512	/* entries kept (oldest are replaced) */
#define DRC_MAXREPLY	1024	/* larger replies are not kept */
#define DRC_TTL		120	/* seconds */

struct drc_key {
	struct in_addr	addr;
	uint16_t	port;
	uint32_t	xid, prog, vers, proc;
	uint32_t	chk;		/* hash of the call message */
};

/* Hash of a call message (detects XID reuse with a different call). */
uint32_t drc_checksum(const void *msg, size_t len);

/*
 * If this call was answered before: copy the reply to "out" (at most
 * "outmax" bytes), set *len, return 1. Otherwise return 0.
 */
int  drc_lookup(const struct drc_key *k, time_t now, void *out,
    size_t outmax, size_t *len);

/* Keep the reply of a finished call. */
void drc_store(const struct drc_key *k, time_t now, const void *reply,
    size_t len);

/* Forget everything (tests, config reload). */
void drc_clear(void);

#endif
