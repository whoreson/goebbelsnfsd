#include <string.h>

#include "drc.h"

struct slot {
	int		used;
	time_t		when;
	struct drc_key	key;
	size_t		len;
	unsigned char	reply[DRC_MAXREPLY];
};

static struct slot slots[DRC_SLOTS];
static unsigned next_slot;	/* FIFO: the next slot to overwrite */

uint32_t
drc_checksum(const void *msg, size_t len)
{
	const unsigned char *p = msg;
	uint32_t h = 2166136261u;	/* FNV-1a */
	size_t i;

	for (i = 0; i < len; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static int
same_key(const struct drc_key *a, const struct drc_key *b)
{
	return a->addr.s_addr == b->addr.s_addr && a->port == b->port &&
	    a->xid == b->xid && a->prog == b->prog && a->vers == b->vers &&
	    a->proc == b->proc && a->chk == b->chk;
}

int
drc_lookup(const struct drc_key *k, time_t now, void *out, size_t outmax,
    size_t *len)
{
	unsigned i;

	for (i = 0; i < DRC_SLOTS; i++) {
		struct slot *s = &slots[i];

		if (!s->used || !same_key(&s->key, k))
			continue;
		if (now - s->when > DRC_TTL) {
			s->used = 0;
			return 0;
		}
		if (s->len > outmax)
			return 0;
		memcpy(out, s->reply, s->len);
		*len = s->len;
		return 1;
	}
	return 0;
}

void
drc_store(const struct drc_key *k, time_t now, const void *reply, size_t len)
{
	struct slot *s;
	unsigned i;

	if (len > DRC_MAXREPLY)
		return;
	/* Reuse the slot of the same call, else take the oldest. */
	for (i = 0; i < DRC_SLOTS; i++)
		if (slots[i].used && same_key(&slots[i].key, k))
			break;
	if (i == DRC_SLOTS) {
		i = next_slot;
		next_slot = (next_slot + 1) % DRC_SLOTS;
	}
	s = &slots[i];
	s->used = 1;
	s->when = now;
	s->key = *k;
	s->len = len;
	memcpy(s->reply, reply, len);
}

void
drc_clear(void)
{
	memset(slots, 0, sizeof(slots));
	next_slot = 0;
}
