#include "port.h"
#include <stdlib.h>
#include <string.h>

#include "dirlist.h"

#define COOKIE_MIN	3			/* 0 = start; 1, 2 kept free */

uint64_t
dl_name_cookie(const char *name, unsigned bits)
{
	uint64_t h = 14695981039346656037ULL;	/* FNV-1a, 64 bit */
	const unsigned char *p = (const unsigned char *)name;

	for (; *p != '\0'; p++)
		h = (h ^ *p) * 1099511628211ULL;
	h &= (1ULL << bits) - 1;
	if (h < COOKIE_MIN)
		h += COOKIE_MIN;
	return h;
}

void
dl_init(struct dirlist *l, unsigned bits)
{
	l->e = NULL;
	l->n = l->cap = 0;
	l->bits = bits;
}

void
dl_free(struct dirlist *l)
{
	free(l->e);
	dl_init(l, l->bits);
}

int
dl_add(struct dirlist *l, uint64_t inode, const char *name)
{
	struct dl_entry *ne;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 64;

		ne = realloc(l->e, cap * sizeof(*ne));
		if (ne == NULL)
			return -1;
		l->e = ne;
		l->cap = cap;
	}
	ne = &l->e[l->n++];
	ne->inode = inode;
	ne->cookie = dl_name_cookie(name, l->bits);
	strlcpy(ne->name, name, sizeof(ne->name));
	return 0;
}

static int
cmp_entry(const void *a, const void *b)
{
	const struct dl_entry *x = a, *y = b;

	if (x->cookie != y->cookie)
		return x->cookie < y->cookie ? -1 : 1;
	return strcmp(x->name, y->name);
}

void
dl_finish(struct dirlist *l)
{
	size_t i;

	if (l->n > 1)
		qsort(l->e, l->n, sizeof(*l->e), cmp_entry);
	/* Equal hashes: make the cookies strictly increasing. */
	for (i = 1; i < l->n; i++)
		if (l->e[i].cookie <= l->e[i - 1].cookie)
			l->e[i].cookie = l->e[i - 1].cookie + 1;
}

size_t
dl_after(const struct dirlist *l, uint64_t cookie)
{
	size_t lo = 0, hi = l->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (l->e[mid].cookie > cookie)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}
