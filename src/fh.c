#include <string.h>
#include <fcntl.h>
#ifdef __FreeBSD__
#include <sys/mount.h>
#include <sys/param.h>
#endif
#include "fh.h"
#include "conf.h"
#include "port.h"

/* Simple LRU path cache for symlinks */
#define FH_PATH_CACHE_SIZE 64
static struct {
	struct nfs_fh key;
	char path[256];
} fh_path_cache[FH_PATH_CACHE_SIZE];

void
fh_path_cache_add(const struct nfs_fh *nfh, const char *path)
{
	size_t i, empty = (size_t)-1;
	/* Check if already exists */
	for (i = 0; i < FH_PATH_CACHE_SIZE; i++) {
	if (fh_path_cache[i].path[0] == '\0' && empty == (size_t)-1)
	empty = i;
	if (memcmp(&fh_path_cache[i].key, nfh, sizeof(*nfh)) == 0) {
	strlcpy(fh_path_cache[i].path, path, sizeof(fh_path_cache[i].path));
	return;
	}
	}
	/* Add new entry */
	if (empty == (size_t)-1)
	empty = 0; /* Evict oldest */
	memcpy(&fh_path_cache[empty].key, nfh, sizeof(*nfh));
	strlcpy(fh_path_cache[empty].path, path, sizeof(fh_path_cache[empty].path));
}

const char *
fh_path_cache_get(const struct nfs_fh *nfh)
{
	size_t i;
	for (i = 0; i < FH_PATH_CACHE_SIZE; i++) {
	if (memcmp(&fh_path_cache[i].key, nfh, sizeof(*nfh)) == 0)
	return fh_path_cache[i].path;
	}
	return NULL;
}

#ifndef __FreeBSD__
const char *
fh_path_cache_getbyfh(const fhandle_t *fh)
{
	size_t i;
	fhandle_t cfh;
	for (i = 0; i < FH_PATH_CACHE_SIZE; i++) {
	if (fh_path_cache[i].path[0] == '\0')
	continue;
	if (fh_decode(&fh_path_cache[i].key, &cfh) < 0)
	continue;
	if (cfh.fh_dev == fh->fh_dev && cfh.fh_ino == fh->fh_ino &&
	    cfh.fh_fsid[0] == fh->fh_fsid[0] &&
	    cfh.fh_fsid[1] == fh->fh_fsid[1])
	return fh_path_cache[i].path;
	}
	return NULL;
}
#endif

int
fh_encode(struct nfs_fh *nfh, const fhandle_t *fh)
{
	uint8_t *p = nfh->data;
	uint16_t v16;
	uint32_t v32;

	memset(nfh, 0, sizeof(*nfh));
	/* magic (2 bytes, big-endian) */
	v16 = NFS_FH_MAGIC;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
#ifdef __FreeBSD__
	/* fid_len (2 bytes, big-endian) */
	v16 = (uint16_t)fh->fh_fid.fid_len;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
	/* fid_data0 (2 bytes, big-endian) - required for ZFS/UFS */
	v16 = (uint16_t)fh->fh_fid.fid_data0;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
	/* fsid[0] (4 bytes, big-endian) */
	v32 = fh->fh_fsid.val[0];
#else
	/* Linux: synthetic fh - store dev(4) + ino(8) + fsid(8) */
	v32 = (uint32_t)fh->fh_dev;
#endif
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
#ifdef __FreeBSD__
	/* fsid[1] (4 bytes, big-endian) */
	v32 = fh->fh_fsid.val[1];
#else
	v32 = (uint32_t)(fh->fh_fsid[0] >> 32);
#endif
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
#ifdef __FreeBSD__
	/* fid (20 bytes) */
	memcpy(p, fh->fh_fid.fid_data, fh->fh_fid.fid_len);
#else
	/* Linux: fsid[0] low 32 bits + ino (8 bytes) */
	v32 = (uint32_t)fh->fh_fsid[0];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	v32 = (uint32_t)(fh->fh_ino >> 32);
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	v32 = (uint32_t)fh->fh_ino;
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
#endif
	return 0;
}

int
fh_valid(const struct nfs_fh *nfh)
{
	const uint8_t *p = nfh->data;
	uint16_t magic, fid_len;

	magic = ((uint16_t)p[0] << 8) | p[1];
	if (magic != NFS_FH_MAGIC)
	return 0;
	fid_len = ((uint16_t)p[2] << 8) | p[3];
	if (fid_len == 0 || fid_len > MAXFIDSZ)
	return 0;
	return 1;
}

int
fh_decode(const struct nfs_fh *nfh, fhandle_t *fh)
{
	const uint8_t *p;
	uint32_t v32;

	memset(fh, 0, sizeof(*fh));
#ifdef __FreeBSD__
	/* fid_len */
	fh->fh_fid.fid_len = (u_short)(((uint16_t)nfh->data[2] << 8) | nfh->data[3]);
	/* fid_data0 */
	fh->fh_fid.fid_data0 = (u_short)(((uint16_t)nfh->data[4] << 8) | nfh->data[5]);
	/* fsid[0] */
	p = nfh->data + 6;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[0] = v32;
	p += 4;
	/* fsid[1] */
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[1] = v32;
	p += 4;
	/* fid_data */
	memcpy(fh->fh_fid.fid_data, p, fh->fh_fid.fid_len);
#else
	/* Linux: dev(4) + fsid_high(4) + fsid_low(4) + ino_high(4) + ino_low(4) */
	p = nfh->data + 6;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_dev = v32;
	p += 4;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid[0] = ((uint64_t)v32) << 32;
	p += 4;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid[0] |= v32;
	p += 4;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_ino = ((uint64_t)v32) << 32;
	p += 4;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_ino |= v32;
#endif
	return 0;
}

const struct export *
fh_lookup_export(const struct nfs_fh *nfh)
{
	const uint8_t *p = nfh->data + 6; /* skip magic(2)+fid_len(2)+fid_data0(2) */
	unsigned i;
	const struct export *ex;
	uint32_t fsid0, fsid1;

	fsid0 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fsid1 = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
	    ((uint32_t)p[6] << 8) | (uint32_t)p[7];

	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (ex->fsid_val[0] == fsid0 && ex->fsid_val[1] == fsid1)
	return ex;
	}

	/*
	 * fsid didn't match any export root. This can happen when the file
	 * is on a different filesystem (e.g., ZFS dataset) mounted under
	 * the export point. Try to resolve the path from the file handle
	 * and find an export that covers it.
	 */
	{
	fhandle_t fh;
	char path[512];
	int fd;

	if (fh_decode(nfh, &fh) < 0)
	return NULL;

	/* First try path cache (fast path for previously seen fhs) */
	{
	const char *cpath = fh_path_cache_get(nfh);
	if (cpath != NULL) {
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (strncmp(cpath, ex->path, strlen(ex->path)) == 0) {
	if (cpath[strlen(ex->path)] == '\0' ||
	    cpath[strlen(ex->path)] == '/')
	return ex;
	}
	}
	}
	}

	/* Fallback: resolve path via fhopen+fchdir+getcwd */
	fd = PORT_FHOPEN(&fh, O_RDONLY);
	if (fd < 0)
	return NULL;
	if (fchdir(fd) == 0 && getcwd(path, sizeof(path)) != NULL) {
	(void)close(fd);
	/* Check if path is under any export */
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (strncmp(path, ex->path, strlen(ex->path)) == 0) {
	if (path[strlen(ex->path)] == '\0' ||
	    path[strlen(ex->path)] == '/')
	return ex;
	}
	}
	} else {
	/* fchdir failed (not a directory). Try parent via fhstat. */
	/* For regular files, we can't fchdir. Check if fhstat works
	   to verify the handle is valid, then fall back to checking
	   if the fsid is a known sub-mount of any export. */
	(void)close(fd);
	}
	}
	return NULL;
}
