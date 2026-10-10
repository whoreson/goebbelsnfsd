/*
 * End-to-end: NFSv2 READDIR while the directory shrinks, and for a big
 * directory without any change (the old server sent inode numbers as
 * cookies and used the next offset as an entry count: it cut the listing
 * off after the first batch).
 * Model of "rsync, then rm -rf": the client reads a page, removes what it
 * got, and continues with the last cookie. The server must still return
 * the rest. (Before the fix it answered "0 entries, eof" and the client
 * thought the directory was empty.) Linux build only.
 */
#include "port.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "conf.h"
#include "fh.h"
#include "progs.h"
#include "rpc.h"
#include "xdr.h"

#define NFILES	300
#define PROC_READDIR		16

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

static char base[64], dir[96];
static struct nfs_fh dirfh;
static const struct nfs_fh *curfh;	/* the directory the next call lists */
#define NDUMMY	300
static struct nfs_fh dummyfh[NDUMMY];
static int seen[NFILES];

/*
 * One call. Returns the number of entries; sets *last (cookie of the last
 * entry) and *eof. Entries are marked in seen[] and removed from the
 * directory like "rm -rf" does.
 */
static int
call(int plus_unused, uint64_t cookie, uint64_t *last, int *eof, int remove_them)
{
	unsigned char abuf[256], rbuf[65536];
	struct req r;
	struct xdr rd;
	int n = 0;
	uint32_t st;

	memset(&r, 0, sizeof(r));
	xdr_init(&r.in, abuf, sizeof(abuf));
	xdr_put_fixed(&r.in, curfh, NFS_FH_SIZE);
	xdr_put_u32(&r.in, (uint32_t)cookie);
	xdr_put_u32(&r.in, 1024);		/* count: reply size in bytes */
	xdr_init(&r.in, abuf, xdr_pos(&r.in));	/* rewind for reading */
	xdr_init(&r.out, rbuf, sizeof(rbuf));
	r.peer.sin_family = AF_INET;
	inet_pton(AF_INET, "10.0.0.5", &r.peer.sin_addr);
	conf_set_client(r.peer.sin_addr);

	(void)nfs2_prog.procs[PROC_READDIR].fn(&r);

	xdr_init(&rd, rbuf, xdr_pos(&r.out));
	st = xdr_get_u32(&rd);
	if (st != 0) {
		printf("status %u\n", st);
		*eof = 1;
		return -1;
	}
	while (xdr_get_u32(&rd) == 1) {
		char name[300], path[400];
		int idx;

		(void)xdr_get_u32(&rd);		/* fileid */
		xdr_get_string(&rd, name, sizeof(name) - 1);
		*last = xdr_get_u32(&rd);	/* cookie */
		idx = atoi(name + 5);
		CHECK(strncmp(name, "file_", 5) == 0 && idx >= 0 &&
		    idx < NFILES);
		if (idx >= 0 && idx < NFILES) {
			seen[idx]++;
			if (remove_them) {
				snprintf(path, sizeof(path), "%s/%s", dir, name);
				(void)unlink(path);
			}
		}
		n++;
	}
	*eof = (int)xdr_get_u32(&rd);
	return n;
}

/*
 * List many other directories. This pushes the listing of "dir" out of
 * the server's cache, so the next request has to read the directory
 * again and sees the files that were removed in the meantime.
 */
static void
thrash(void)
{
	const struct nfs_fh *save = curfh;
	uint64_t last;
	int i, eof;

	for (i = 0; i < NDUMMY; i++) {
		curfh = &dummyfh[i];
		(void)call(0, 0, &last, &eof, 0);
	}
	curfh = save;
}

static int
count_dir(void)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			n++;
	closedir(d);
	return n;
}

static void
scenario(int remove, int thrash_between)
{
	char path[400];
	uint64_t cookie = 0, last = 0;
	int i, eof = 0, n, rounds = 0, total = 0;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < NFILES; i++) {
		int fd;

		snprintf(path, sizeof(path), "%s/file_%03d", dir, i);
		fd = open(path, O_CREAT | O_WRONLY, 0644);
		close(fd);
	}
	curfh = &dirfh;

	do {
		if (thrash_between && cookie != 0)
			thrash();
		n = call(0, cookie, &last, &eof, remove);
		CHECK(n >= 0);
		if (n <= 0 && !eof)
			break;
		total += n > 0 ? n : 0;
		cookie = last;
		rounds++;
	} while (!eof && rounds < 1000);

	CHECK(rounds > 3);			/* several pages were needed */
	CHECK(total == NFILES);
	for (i = 0; i < NFILES; i++)
		CHECK(seen[i] == 1);
	if (remove) {
		CHECK(count_dir() == 0);	/* nothing was left behind */
		/* A new listing of the empty directory: no entries, eof. */
		n = call(0, 0, &last, &eof, 0);
		CHECK(n == 0 && eof == 1);
	} else {
		CHECK(count_dir() == NFILES);
		for (i = 0; i < NFILES; i++) {
			snprintf(path, sizeof(path), "%s/file_%03d", dir, i);
			(void)unlink(path);
		}
	}
}

int
main(void)
{
#ifdef __FreeBSD__
	return 0;
#else
	char exp[128], cmd[160];
	fhandle_t h;
	FILE *f;

	snprintf(base, sizeof(base), "/tmp/rd3testXXXXXX");
	CHECK(mkdtemp(base) != NULL);
	snprintf(dir, sizeof(dir), "%s/t", base);
	CHECK(mkdir(dir, 0755) == 0);
	snprintf(exp, sizeof(exp), "%s.exports", base);
	f = fopen(exp, "w");
	fprintf(f, "%s -maproot=root\n", base);
	fclose(f);
	CHECK(conf_load(exp) == 0);

	/* handles, with paths in the cache as LOOKUP would leave them */
	CHECK(port_lgetfh(dir, &h) == 0);
	fh_encode(&dirfh, &h);
	fh_path_cache_add(&dirfh, dir);
	int _i;
	for (_i = 0; _i < NDUMMY; _i++) {
		char dp[128];

		snprintf(dp, sizeof(dp), "%s/d%03d", base, i);
		CHECK(mkdir(dp, 0755) == 0);
		CHECK(port_lgetfh(dp, &h) == 0);
		fh_encode(&dummyfh[i], &h);
		fh_path_cache_add(&dummyfh[i], dp);
	}

	scenario(0, 0);		/* big directory, no change */
	scenario(1, 0);		/* rm between the pages */
	scenario(0, 1);		/* no change, cache evicted */
	scenario(1, 1);		/* rm between the pages, cache evicted */

	snprintf(cmd, sizeof(cmd), "rm -rf %s %s", base, exp);
	(void)system(cmd);
	if (fails == 0)
		printf("readdir2_test: OK\n");
	return fails != 0;
#endif
}
