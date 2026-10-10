#ifndef DIRLIST_H
#define DIRLIST_H

/*
 * Directory listing with stable READDIR cookies.
 *
 * The cookie of an entry is a hash of its name, not its position.
 * A listing is sorted by cookie. A request with cookie C continues at
 * the first entry whose cookie is greater than C.
 *
 * Because of this, a continuation request is correct even if the
 * listing was built again after files were created or removed: an
 * entry that is not changed keeps its cookie, and no other entry can
 * move in front of it. (An index into a changing array cannot do this:
 * after "rm" the index is past the end, the server sends eof, and the
 * client thinks the directory is empty.)
 *
 * Cookie 0 means "start". Every cookie is > 2 and fits in "bits" bits
 * (62 for NFSv3, so it is safe as a signed 64-bit file offset in the Linux
 * client; 31 for NFSv2, whose cookie has only 32 bits).
 *
 * A shorter cookie has more hash collisions. Colliding names get
 * consecutive cookies. If one of two colliding names is removed between
 * two calls, the other one can be missed. With 62 bits this does not
 * happen in practice; with 31 bits it needs two names with the same hash.
 */
#define DL_BITS_V3	62
#define DL_BITS_V2	31

#include <stddef.h>
#include <stdint.h>

#define DL_NAMEMAX	256

struct dl_entry {
	uint64_t cookie;
	uint64_t inode;
	char     name[DL_NAMEMAX];
};

struct dirlist {
	struct dl_entry *e;
	size_t           n, cap;
	unsigned         bits;	/* width of the cookies */
};

/* The cookie that belongs to a name (before collision handling). */
uint64_t dl_name_cookie(const char *name, unsigned bits);

void dl_init(struct dirlist *l, unsigned bits);
void dl_free(struct dirlist *l);

/* Add one entry. Return 0, or -1 if out of memory. "." and ".." are ignored. */
int  dl_add(struct dirlist *l, uint64_t inode, const char *name);

/*
 * Sort by cookie and make the cookies unique (two names with the same
 * hash get consecutive cookies, ordered by name).
 */
void dl_finish(struct dirlist *l);

/* Index of the first entry with cookie > "cookie" (n if none). */
size_t dl_after(const struct dirlist *l, uint64_t cookie);

#endif
