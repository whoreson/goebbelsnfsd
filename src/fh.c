#include "port.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __FreeBSD__
#include <sys/mount.h>
#include <sys/param.h>
#endif
#include <string.h>
#include "fh.h"
#include "conf.h"
#include "port.h"
#include "log.h"

/*
 * Path cache: file handle -> path.
 *
 * A handle does not carry a path, and fhopen() follows symlinks, so the
 * server remembers the path it saw at LOOKUP/CREATE time. The cache is a
 * hash table (O(1) lookup) with a real LRU list. It holds at most
 * FH_PATH_CACHE_SIZE entries. Paths are stored without a length limit
 * other than PATH_MAX: a longer path is not cached (never truncated).
 *
 * A cached path can become wrong after RENAME or REMOVE. The handlers
 * call fh_path_cache_rename() and fh_path_cache_forget() to fix that.
 */
#define PC_BUCKETS	2048u	/* power of 2 */

struct pc_ent {
	struct nfs_fh	key;
	char		*path;
	struct pc_ent	*hnext;			/* hash chain */
	struct pc_ent	*lprev, *lnext;		/* LRU list: head = newest */
	unsigned	bucket;
};

static struct pc_ent *pc_tab[PC_BUCKETS];
static struct pc_ent *pc_head, *pc_tail;
static unsigned pc_count;

static unsigned
pc_hash(const struct nfs_fh *k)
{
	uint32_t h = 2166136261u;	/* FNV-1a */
	size_t i;

	for (i = 0; i < sizeof(k->data); i++)
		h = (h ^ k->data[i]) * 16777619u;
	return h & (PC_BUCKETS - 1);
}

static void
pc_lru_unlink(struct pc_ent *e)
{
	if (e->lprev != NULL)
		e->lprev->lnext = e->lnext;
	else
		pc_head = e->lnext;
	if (e->lnext != NULL)
		e->lnext->lprev = e->lprev;
	else
		pc_tail = e->lprev;
	e->lprev = e->lnext = NULL;
}

static void
pc_lru_push(struct pc_ent *e)
{
	e->lprev = NULL;
	e->lnext = pc_head;
	if (pc_head != NULL)
		pc_head->lprev = e;
	pc_head = e;
	if (pc_tail == NULL)
		pc_tail = e;
}

static struct pc_ent *
pc_find(const struct nfs_fh *k)
{
	struct pc_ent *e;

	for (e = pc_tab[pc_hash(k)]; e != NULL; e = e->hnext)
		if (memcmp(&e->key, k, sizeof(*k)) == 0)
			return e;
	return NULL;
}

static void
pc_free(struct pc_ent *e)
{
	struct pc_ent **pp;

	for (pp = &pc_tab[e->bucket]; *pp != NULL; pp = &(*pp)->hnext)
		if (*pp == e) {
			*pp = e->hnext;
			break;
		}
	pc_lru_unlink(e);
	free(e->path);
	free(e);
	pc_count--;
}

void
fh_path_cache_add(const struct nfs_fh *nfh, const char *path)
{
	struct pc_ent *e;
	char *copy;

	if (strlen(path) >= PATH_MAX)
		return;		/* never store a truncated path */
	copy = strdup(path);
	if (copy == NULL)
		return;
	e = pc_find(nfh);
	if (e != NULL) {
		free(e->path);
		e->path = copy;
		pc_lru_unlink(e);
		pc_lru_push(e);
		return;
	}
	if (pc_count >= FH_PATH_CACHE_SIZE && pc_tail != NULL)
		pc_free(pc_tail);	/* evict the least recently used */
	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		free(copy);
		return;
	}
	e->key = *nfh;
	e->path = copy;
	e->bucket = pc_hash(nfh);
	e->hnext = pc_tab[e->bucket];
	pc_tab[e->bucket] = e;
	pc_lru_push(e);
	pc_count++;
}

/* The returned pointer stays valid until the next cache change. */
const char *
fh_path_cache_get(const struct nfs_fh *nfh)
{
	struct pc_ent *e = pc_find(nfh);

	if (e == NULL)
		return NULL;
	if (e != pc_head) {
		pc_lru_unlink(e);
		pc_lru_push(e);
	}
	return e->path;
}

void
fh_path_cache_add_bypath(const fhandle_t *fh, const char *path)
{
	struct nfs_fh tmp;

	fh_encode(&tmp, fh);
	fh_path_cache_add(&tmp, path);
}

/* The key is the encoded handle: no scan, no decode of every entry. */
const char *
fh_path_cache_getbyfh(const fhandle_t *fh)
{
	struct nfs_fh tmp;

	fh_encode(&tmp, fh);
	return fh_path_cache_get(&tmp);
}

/* Return 1 if "p" is "base" or is below "base". */
static int
path_under(const char *p, const char *base)
{
	size_t n = strlen(base);

	return strncmp(p, base, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

void
fh_path_cache_forget(const char *path)
{
	struct pc_ent *e, *next;

	for (e = pc_head; e != NULL; e = next) {
		next = e->lnext;
		if (path_under(e->path, path))
			pc_free(e);
	}
}

void
fh_path_cache_rename(const char *from, const char *to)
{
	struct pc_ent *e, *next;
	size_t flen = strlen(from);
	char buf[PATH_MAX];

	/* The target name, if it existed, is gone. */
	fh_path_cache_forget(to);
	for (e = pc_head; e != NULL; e = next) {
		next = e->lnext;
		if (!path_under(e->path, from))
			continue;
		if (snprintf(buf, sizeof(buf), "%s%s", to, e->path + flen) >=
		    (int)sizeof(buf)) {
			pc_free(e);	/* cannot be rewritten: drop it */
			continue;
		}
		free(e->path);
		e->path = strdup(buf);
		if (e->path == NULL)
			pc_free(e);	/* pc_free handles NULL path */
	}
}

#ifdef __FreeBSD__
/*
 * Ask the kernel for the current path of a directory handle.
 * fhopen()+fchdir()+getcwd() works for directories only, and fhopen()
 * follows symlinks, so do not use it for handles of symlinks.
 */
static int
kernel_dirpath(const struct nfs_fh *nfh, char *buf, size_t sz)
{
	fhandle_t fh;
	int fd, ok;

	if (fh_decode(nfh, &fh) < 0)
		return -1;
	fd = PORT_FHOPEN(&fh, O_RDONLY);
	if (fd < 0)
		return -1;
	ok = (fchdir(fd) == 0 && getcwd(buf, sz) != NULL);
	(void)close(fd);
	(void)chdir("/");	/* undo fchdir(): keep a fixed working dir */
	return ok ? 0 : -1;
}
#endif

static int
cache_path(const struct nfs_fh *nfh, char *buf, size_t sz)
{
	const char *c = fh_path_cache_get(nfh);

	if (c == NULL || strlcpy(buf, c, sz) >= sz)
		return -1;
	return 0;
}

/* Files and symlinks: the cache is the only exact source. */
int
fh_resolve_path(const struct nfs_fh *nfh, char *buf, size_t sz)
{
	if (cache_path(nfh, buf, sz) == 0)
		return 0;
#ifdef __FreeBSD__
	if (kernel_dirpath(nfh, buf, sz) == 0) {
		fh_path_cache_add(nfh, buf);
		return 0;
	}
#endif
	return -1;
}

/* Directories: the kernel is exact, the cache can be old after RENAME. */
int
fh_resolve_dirpath(const struct nfs_fh *nfh, char *buf, size_t sz)
{
#ifdef __FreeBSD__
	if (kernel_dirpath(nfh, buf, sz) == 0)
		return 0;
#endif
	return cache_path(nfh, buf, sz);
}

/*
 * Unified wire format (both platforms):
 *   offset 0: magic (2 bytes)
 *   offset 2: fsid[0] (4 bytes)
 *   offset 6: fsid[1] (4 bytes)
 *   offset 10: platform-specific data (dev/ino on Linux, fid on FreeBSD)
 *
 * This ensures fh_lookup_export can always read fsid at offset 2+6.
 */
static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8) & 0xFF;
	p[3] = v & 0xFF;
}

static uint32_t
get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int
fh_encode(struct nfs_fh *nfh, const fhandle_t *fh)
{
	uint8_t *p = nfh->data;
	uint16_t v16;
#ifdef __FreeBSD__
	uint32_t v32;
#endif

	memset(nfh, 0, sizeof(*nfh));
	/* magic (2 bytes, big-endian) */
	v16 = NFS_FH_MAGIC;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
#ifdef __FreeBSD__
	/* FreeBSD: fid_len(2) + fid_data0(2) + fsid0(4) + fsid1(4) + fid_data */
	v16 = (uint16_t)fh->fh_fid.fid_len;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
	v16 = (uint16_t)fh->fh_fid.fid_data0;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
	v32 = fh->fh_fsid.val[0];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	v32 = fh->fh_fsid.val[1];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	memcpy(p, fh->fh_fid.fid_data, fh->fh_fid.fid_len);
#else
	/* Linux: fsid0(4) + fsid1(4) + dev(4) + ino(8) */
	put_be32(p, fh->fh_fsid[0]);
	put_be32(p + 4, fh->fh_fsid[1]);
	put_be32(p + 8, (uint32_t)fh->fh_dev);
	put_be32(p + 12, (uint32_t)(fh->fh_ino >> 32));
	put_be32(p + 16, (uint32_t)fh->fh_ino);
#endif
	return 0;
}

int
fh_valid(const struct nfs_fh *nfh)
{
	const uint8_t *p = nfh->data;
	uint16_t magic;

	magic = ((uint16_t)p[0] << 8) | p[1];
	if (magic != NFS_FH_MAGIC)
	return 0;
#ifdef __FreeBSD__
	{
	uint16_t fid_len = ((uint16_t)nfh->data[2] << 8) | nfh->data[3];
	if (fid_len == 0 || fid_len > MAXFIDSZ)
	return 0;
	}
#endif
	return 1;
}

int
fh_decode(const struct nfs_fh *nfh, fhandle_t *fh)
{
	const uint8_t *p;
#ifdef __FreeBSD__
	uint32_t v32;
#endif

	memset(fh, 0, sizeof(*fh));
#ifdef __FreeBSD__
	/* FreeBSD: fid_len(2) + fid_data0(2) + fsid0(4) + fsid1(4) + fid_data */
	fh->fh_fid.fid_len = (u_short)(((uint16_t)nfh->data[2] << 8) | nfh->data[3]);
	fh->fh_fid.fid_data0 = (u_short)(((uint16_t)nfh->data[4] << 8) | nfh->data[5]);
	p = nfh->data + 6;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[0] = v32;
	p = nfh->data + 10;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[1] = v32;
	memcpy(fh->fh_fid.fid_data, nfh->data + 14, fh->fh_fid.fid_len);
#else
	/* Linux: fsid0(4) + fsid1(4) + dev(4) + ino(8) */
	p = nfh->data + 2;
	fh->fh_fsid[0] = get_be32(p);
	fh->fh_fsid[1] = get_be32(p + 4);
	fh->fh_dev = get_be32(p + 8);
	fh->fh_ino = ((uint64_t)get_be32(p + 12) << 32) | get_be32(p + 16);
#endif
	return 0;
}

/* An export is usable only for the client that sent the current request. */
static int
ex_usable(const struct export *ex)
{
	return conf_client_ok(ex, conf_get_client());
}

int
fh_export_ro(const struct nfs_fh *nfh)
{
	const struct export *ex = fh_lookup_export(nfh);

	return ex == NULL || ex->ro;
}

const struct export *
fh_lookup_export(const struct nfs_fh *nfh)
{
	unsigned i;
	const struct export *ex;
	const uint8_t *p;

#ifdef __FreeBSD__
	/* FreeBSD: fsid at offset 6 (after magic(2)+fid_len(2)+fid_data0(2)) */
	p = nfh->data + 6;
#else
	/* Linux: fsid at offset 2 (after magic(2)) */
	p = nfh->data + 2;
#endif
	{
	uint32_t fsid0 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	uint32_t fsid1 = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
	    ((uint32_t)p[6] << 8) | (uint32_t)p[7];
	const char *known = fh_path_cache_get(nfh);

	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (ex->fsid_val[0] != fsid0 || ex->fsid_val[1] != fsid1 ||
	    !ex_usable(ex))
	continue;
	/*
	 * Same filesystem is not enough: other files on it are not exported.
	 * If the path of the handle is known, it must be inside the export.
	 * (An unknown path cannot be checked here.)
	 */
	if (known != NULL && !path_under(known, ex->path))
	continue;
	return ex;
	}
	}

	/* Try path cache (handles from this server session) */
	{
	const char *cpath = fh_path_cache_get(nfh);
	if (cpath != NULL) {
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (strncmp(cpath, ex->path, strlen(ex->path)) == 0 &&
	    ex_usable(ex)) {
	if (cpath[strlen(ex->path)] == '\0' ||
	    cpath[strlen(ex->path)] == '/')
	return ex;
	}
	}
	}
	}

	/*
	 * The fsid is not the root fsid of any export and the path cache
	 * had nothing. Ask for the path (directories only) and find an export
	 * that covers it. This does not change the working directory.
	 */
	{
	char path[PATH_MAX];

	if (fh_resolve_dirpath(nfh, path, sizeof(path)) == 0) {
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (path_under(path, ex->path) && ex_usable(ex))
	return ex;
	}
	}
	}

	return NULL;
}
