/*
 * READDIR cookies must survive a directory that changes between calls.
 * Model of the reported failure: rsync fills a directory, "rm -rf" reads
 * a page, deletes files, and sends the next request with the old cookie.
 */
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dirlist.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

#define N 1000
static char names[N + 200][32];
static int present[N + 200];

/* Build the listing from the current "directory" (a fresh build). */
static void
build(struct dirlist *l)
{
	int i;

	dl_init(l, DL_BITS_V3);
	for (i = 0; i < N + 200; i++)
		if (present[i])
			dl_add(l, (uint64_t)i + 10, names[i]);
	dl_finish(l);
}

/* One READDIR call: up to "page" entries after "cookie". */
static size_t
page(const struct dirlist *l, uint64_t cookie, size_t pg, uint64_t *last,
    int *seen)
{
	size_t i = dl_after(l, cookie), n = 0;

	for (; i < l->n && n < pg; i++, n++) {
		int idx = atoi(l->e[i].name + 1);

		seen[idx]++;
		*last = l->e[i].cookie;
	}
	return n;
}

int
main(void)
{
	struct dirlist l;
	static int seen[N + 200];
	uint64_t cookie = 0;
	int i, round;
	size_t n;

	for (i = 0; i < N + 200; i++) {
		snprintf(names[i], sizeof(names[i]), "f%d", i);
		present[i] = i < N;
	}

	/* 1. Cookies are unique, increasing, > 2, < 2^62. */
	build(&l);
	CHECK(l.n == N);
	for (i = 0; i < (int)l.n; i++) {
		CHECK(l.e[i].cookie > 2 && l.e[i].cookie < (1ULL << 62));
		if (i > 0)
			CHECK(l.e[i].cookie > l.e[i - 1].cookie);
	}
	/* "." and ".." are never listed */
	CHECK(dl_add(&l, 1, ".") == 0 && dl_add(&l, 1, "..") == 0 && l.n == N);
	dl_free(&l);

	/* 2. THE BUG: read a page, delete the entries already seen, build the
	 * listing again, continue with the old cookie. Every entry that was
	 * not deleted must still be returned. */
	memset(seen, 0, sizeof(seen));
	build(&l);
	n = page(&l, 0, 100, &cookie, seen);
	CHECK(n == 100);
	dl_free(&l);
	for (i = 0; i < N; i++)
		if (seen[i])
			present[i] = 0;		/* rm of the first page */
	for (round = 0; round < 50; round++) {
		build(&l);			/* fresh listing: fewer entries */
		n = page(&l, cookie, 100, &cookie, seen);
		dl_free(&l);
		if (n == 0)
			break;
		for (i = 0; i < N; i++)
			if (seen[i] && present[i])
				present[i] = 0;	/* rm everything returned */
	}
	for (i = 0; i < N; i++)
		CHECK(seen[i] == 1);		/* each file seen exactly once */
	build(&l);
	CHECK(l.n == 0);			/* and the directory is empty */
	dl_free(&l);

	/* 3. Entries created during the listing: no entry is returned twice
	 * and no entry that existed at the start is missed. */
	for (i = 0; i < N; i++)
		present[i] = 1;
	memset(seen, 0, sizeof(seen));
	cookie = 0;
	for (round = 0; round < 100; round++) {
		if (round == 1)
			for (i = N; i < N + 200; i++)
				present[i] = 1;	/* new files appear */
		build(&l);
		n = page(&l, cookie, 100, &cookie, seen);
		dl_free(&l);
		if (n == 0)
			break;
	}
	for (i = 0; i < N + 200; i++) {
		CHECK(seen[i] <= 1);
		if (i < N)
			CHECK(seen[i] == 1);
	}

	/* 4. Same hash: both names are kept, cookies stay distinct. */
	dl_init(&l, DL_BITS_V3);
	dl_add(&l, 1, "a");
	dl_add(&l, 2, "b");
	l.e[0].cookie = l.e[1].cookie = 12345;	/* force a collision */
	dl_finish(&l);
	CHECK(l.e[0].cookie != l.e[1].cookie);
	CHECK(dl_after(&l, 0) == 0);
	CHECK(dl_after(&l, l.e[0].cookie) == 1);
	CHECK(dl_after(&l, l.e[1].cookie) == 2);
	dl_free(&l);

	/* 5. Empty directory, and a cookie past the end. */
	dl_init(&l, DL_BITS_V3);
	dl_finish(&l);
	CHECK(dl_after(&l, 0) == 0);
	dl_add(&l, 1, "x");
	dl_finish(&l);
	CHECK(dl_after(&l, l.e[0].cookie) == 1);
	CHECK(dl_after(&l, ~0ULL) == 1);
	dl_free(&l);

	/* 6. NFSv2: 31-bit cookies, all > 2, unique, and still stable. */
	dl_init(&l, DL_BITS_V2);
	for (i = 0; i < N; i++)
		dl_add(&l, (uint64_t)i, names[i]);
	dl_finish(&l);
	for (i = 0; i < (int)l.n; i++) {
		CHECK(l.e[i].cookie > 2 && l.e[i].cookie < (1ULL << 31) + N);
		if (i > 0)
			CHECK(l.e[i].cookie > l.e[i - 1].cookie);
	}
	CHECK(dl_name_cookie("f1", DL_BITS_V2) < (1ULL << 31));
	dl_free(&l);

	if (fails == 0)
		printf("dirlist_test: OK\n");
	return fails != 0;
}
