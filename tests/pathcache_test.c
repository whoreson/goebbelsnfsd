/* Path cache: lookup, LRU eviction, rename, forget. Linux build only. */
#include "port.h"
#include <stdio.h>
#include <string.h>
#include "fh.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

#ifndef __FreeBSD__
static struct nfs_fh
mk(uint64_t ino)
{
	fhandle_t h;
	struct nfs_fh n;

	memset(&h, 0, sizeof(h));
	h.fh_fsid[0] = 1;
	h.fh_fsid[1] = 2;
	h.fh_dev = 3;
	h.fh_ino = ino;
	fh_encode(&n, &h);
	return n;
}
#endif

int
main(void)
{
#ifdef __FreeBSD__
	printf("pathcache_test: skipped on FreeBSD\n");
	return 0;
#else
	struct nfs_fh a = mk(1), b = mk(2), c = mk(3), d = mk(4), x;
	char longp[2000];
	char buf[64];
	unsigned i;

	/* basic add / get / update */
	CHECK(fh_path_cache_get(&a) == NULL);
	fh_path_cache_add(&a, "/e/a");
	CHECK(strcmp(fh_path_cache_get(&a), "/e/a") == 0);
	fh_path_cache_add(&a, "/e/a2");
	CHECK(strcmp(fh_path_cache_get(&a), "/e/a2") == 0);

	/* long paths are kept whole (old cache cut them at 255 bytes) */
	memset(longp, 'x', sizeof(longp) - 1);
	longp[0] = '/';
	longp[sizeof(longp) - 1] = '\0';
	fh_path_cache_add(&b, longp);
	CHECK(fh_path_cache_get(&b) != NULL);
	CHECK(strlen(fh_path_cache_get(&b)) == sizeof(longp) - 1);

	/* resolve copies and refuses a buffer that is too small */
	CHECK(fh_resolve_path(&a, buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "/e/a2") == 0);
	CHECK(fh_resolve_path(&a, buf, 3) < 0);
	CHECK(fh_resolve_path(&d, buf, sizeof(buf)) < 0);

	/* rename rewrites the entry and everything below it */
	fh_path_cache_add(&c, "/e/dir/file");
	fh_path_cache_add(&d, "/e/dirx/file");
	fh_path_cache_rename("/e/dir", "/e/new");
	CHECK(strcmp(fh_path_cache_get(&c), "/e/new/file") == 0);
	CHECK(strcmp(fh_path_cache_get(&d), "/e/dirx/file") == 0);

	/* forget removes the path and its children, not siblings */
	fh_path_cache_forget("/e/new");
	CHECK(fh_path_cache_get(&c) == NULL);
	CHECK(fh_path_cache_get(&d) != NULL);

	/* LRU: fill past the limit. Old unused entries go, used ones stay. */
	fh_path_cache_add(&a, "/keep");
	for (i = 0; i < FH_PATH_CACHE_SIZE + 100; i++) {
		x = mk(1000 + i);
		snprintf(buf, sizeof(buf), "/p/%u", i);
		fh_path_cache_add(&x, buf);
		(void)fh_path_cache_get(&a);	/* keep "a" recent */
	}
	CHECK(fh_path_cache_get(&a) != NULL);
	x = mk(1000);
	CHECK(fh_path_cache_get(&x) == NULL);		/* oldest: evicted */
	x = mk(1000 + FH_PATH_CACHE_SIZE + 99);
	CHECK(fh_path_cache_get(&x) != NULL);		/* newest: present */

	if (fails == 0)
		printf("pathcache_test: OK\n");
	return fails != 0;
#endif
}
