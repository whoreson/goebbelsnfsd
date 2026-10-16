#include "port.h"
#include "inx.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- Build ---- */

struct bent {
	uint64_t dev;
	uint64_t ino;
	size_t   off;		/* offset of path in arena */
};

static struct bent *b_ents;
static size_t b_n, b_cap;
static char *b_arena;
static size_t b_alen, b_acap;

static void
b_reset(void)
{
	free(b_ents);
	free(b_arena);
	b_ents = NULL;
	b_arena = NULL;
	b_n = b_cap = b_alen = b_acap = 0;
}

static int
b_add(const char *path, uint64_t dev, uint64_t ino)
{
	size_t len = strlen(path) + 1;

	if (b_n == b_cap) {
		size_t ncap = b_cap == 0 ? 1024 : b_cap * 2;
		struct bent *p = realloc(b_ents, ncap * sizeof(*p));

		if (p == NULL)
			return -1;
		b_ents = p;
		b_cap = ncap;
	}
	if (b_alen + len > b_acap) {
		size_t ncap = b_acap == 0 ? 65536 : b_acap;
		char *p;

		while (b_alen + len > ncap)
			ncap *= 2;
		p = realloc(b_arena, ncap);
		if (p == NULL)
			return -1;
		b_arena = p;
		b_acap = ncap;
	}
	memcpy(b_arena + b_alen, path, len);
	b_ents[b_n].dev = dev;
	b_ents[b_n].ino = ino;
	b_ents[b_n].off = b_alen;
	b_n++;
	b_alen += len;
	return 0;
}

static int
b_cmp(const void *a, const void *b)
{
	const struct bent *x = a, *y = b;

	if (x->dev != y->dev)
		return x->dev < y->dev ? -1 : 1;
	if (x->ino != y->ino)
		return x->ino < y->ino ? -1 : 1;
	/* Same inode (hard links, repeated exports): sort by path. */
	return strcmp(b_arena + x->off, b_arena + y->off);
}

/*
 * Walk "path" (a directory on device "dev"). Only symlinks are
 * stored. Directories on other devices are not entered.
 * Returns -1 on out-of-memory, else 0.
 */
static int
b_walk(const char *path, uint64_t dev)
{
	DIR *d;
	struct dirent *ent;
	char child[PATH_MAX];
	struct stat sb;
	int rc = 0;

	d = opendir(path);
	if (d == NULL) {
		log_msg(L_WARN, "inx_build: opendir %s: %s", path,
		    strerror(errno));
		return 0;
	}
	while (rc == 0 && (ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path,
		    ent->d_name) >= (int)sizeof(child)) {
			log_msg(L_WARN, "inx_build: path too long below %s",
			    path);
			continue;
		}
		if (lstat(child, &sb) < 0)
			continue;
		if (S_ISLNK(sb.st_mode)) {
			if ((uint64_t)sb.st_dev == dev)
				rc = b_add(child, (uint64_t)sb.st_dev,
				    (uint64_t)sb.st_ino);
		} else if (S_ISDIR(sb.st_mode) && (uint64_t)sb.st_dev == dev) {
			rc = b_walk(child, dev);
		}
	}
	closedir(d);
	return rc;
}

/* Create the parent directory of "file" (one level). */
static void
mk_parent(const char *file)
{
	char dir[PATH_MAX];
	char *slash;

	if (strlen(file) >= sizeof(dir))
		return;
	strcpy(dir, file);
	slash = strrchr(dir, '/');
	if (slash == NULL || slash == dir)
		return;
	*slash = '\0';
	(void)mkdir(dir, 0755);
}

int
inx_build(const char *const *roots, unsigned nroots, const char *outfile)
{
	struct inx_hdr hdr;
	struct inx_rec *recs = NULL;
	char tmp[PATH_MAX];
	FILE *fp = NULL;
	size_t i, n, pool = 0;
	unsigned r, nok = 0;
	int ret = -1;

	b_reset();
	for (r = 0; r < nroots; r++) {
		struct stat sb;

		if (stat(roots[r], &sb) < 0) {
			log_msg(L_WARN, "inx_build: stat %s: %s", roots[r],
			    strerror(errno));
			continue;
		}
		if (b_walk(roots[r], (uint64_t)sb.st_dev) < 0) {
			log_msg(L_ERR, "inx_build: out of memory");
			goto out;
		}
		nok++;
	}
	if (nok == 0) {
		/* Do not replace a good index with an empty one. */
		log_msg(L_ERR, "inx_build: no usable export, index not written");
		goto out;
	}
	if (b_n > 1)
		qsort(b_ents, b_n, sizeof(*b_ents), b_cmp);

	/* Drop exact duplicates (nested exports are walked twice). */
	for (i = n = 0; i < b_n; i++) {
		if (n > 0 && b_ents[n - 1].dev == b_ents[i].dev &&
		    b_ents[n - 1].ino == b_ents[i].ino &&
		    strcmp(b_arena + b_ents[n - 1].off,
		    b_arena + b_ents[i].off) == 0)
			continue;
		b_ents[n++] = b_ents[i];
	}
	b_n = n;

	if (b_n >= UINT32_MAX / sizeof(*recs) || b_alen >= UINT32_MAX) {
		log_msg(L_ERR, "inx_build: index too large");
		goto out;
	}
	recs = malloc((b_n ? b_n : 1) * sizeof(*recs));
	if (recs == NULL)
		goto out;
	for (i = 0; i < b_n; i++) {
		size_t len = strlen(b_arena + b_ents[i].off) + 1;

		recs[i].dev = b_ents[i].dev;
		recs[i].ino = b_ents[i].ino;
		recs[i].off = (uint32_t)pool;
		recs[i].len = (uint32_t)len;
		pool += len;
	}

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", outfile) >= (int)sizeof(tmp))
		goto out;
	mk_parent(outfile);
	fp = fopen(tmp, "wb");
	if (fp == NULL) {
		log_msg(L_WARN, "inx_build: open %s: %s", tmp, strerror(errno));
		goto out;
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = INX_MAGIC;
	hdr.built = (uint64_t)time(NULL);
	hdr.count = (uint32_t)b_n;
	hdr.pool_len = (uint32_t)pool;
	if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1)
		goto werr;
	if (b_n > 0 && fwrite(recs, sizeof(*recs), b_n, fp) != b_n)
		goto werr;
	for (i = 0; i < b_n; i++)
		if (fwrite(b_arena + b_ents[i].off, recs[i].len, 1, fp) != 1)
			goto werr;
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		goto werr;
	if (fclose(fp) != 0) {
		fp = NULL;
		goto werr;
	}
	fp = NULL;
	if (rename(tmp, outfile) < 0) {
		log_msg(L_WARN, "inx_build: rename %s: %s", outfile,
		    strerror(errno));
		(void)unlink(tmp);
		goto out;
	}
	log_msg(L_INFO, "inx_build: %lu symlinks, %lu bytes, %s",
	    (unsigned long)b_n,
	    (unsigned long)(sizeof(hdr) + b_n * sizeof(*recs) + pool), outfile);
	ret = 0;
	goto out;

werr:
	log_msg(L_WARN, "inx_build: write %s: %s", tmp, strerror(errno));
	if (fp != NULL)
		fclose(fp);
	(void)unlink(tmp);
out:
	free(recs);
	b_reset();
	return ret;
}

/* ---- Lookup ---- */

static char *inx_base;			/* mapping, or NULL */
static size_t inx_size;

int
inx_preload(const char *infile)
{
	struct stat sb;
	const struct inx_hdr *h;
	void *map;
	size_t want;
	int fd;

	fd = open(infile, O_RDONLY);
	if (fd < 0) {
		log_msg(L_INFO, "inx: no index %s: %s", infile, strerror(errno));
		return -1;
	}
	if (fstat(fd, &sb) < 0 || sb.st_size < (off_t)sizeof(*h)) {
		close(fd);
		log_msg(L_WARN, "inx: %s: too short", infile);
		return -1;
	}
	map = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return -1;
	h = map;
	want = sizeof(*h) + (size_t)h->count * sizeof(struct inx_rec) +
	    h->pool_len;
	if (h->magic != INX_MAGIC || want != (size_t)sb.st_size) {
		log_msg(L_WARN, "inx: %s: bad magic or size, ignored", infile);
		munmap(map, (size_t)sb.st_size);
		return -1;
	}
	if (inx_base != NULL)
		munmap(inx_base, inx_size);
	inx_base = (char *)map;
	inx_size = (size_t)sb.st_size;
	log_msg(L_INFO, "inx: loaded %u symlinks from %s", h->count, infile);
	return (int)h->count;
}

const char *
inx_lookup(uint64_t dev, uint64_t ino)
{
	const struct inx_hdr *h = (const void *)inx_base;
	const struct inx_rec *recs;
	const char *pool;
	uint32_t lo, hi;

	if (h == NULL)
		return NULL;
	recs = (const struct inx_rec *)(const void *)(inx_base + sizeof(*h));
	pool = (const char *)(recs + h->count);

	/* Lower bound: first record not less than (dev, ino). */
	lo = 0;
	hi = h->count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (recs[mid].dev < dev ||
		    (recs[mid].dev == dev && recs[mid].ino < ino))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= h->count || recs[lo].dev != dev || recs[lo].ino != ino)
		return NULL;
	/* Do not trust the file: check bounds and the NUL byte. */
	if (recs[lo].len == 0 || recs[lo].off > h->pool_len ||
	    recs[lo].len > h->pool_len - recs[lo].off ||
	    pool[recs[lo].off + recs[lo].len - 1] != '\0')
		return NULL;
	return pool + recs[lo].off;
}
