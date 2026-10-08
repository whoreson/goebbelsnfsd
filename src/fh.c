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

/* Simple LRU path cache for symlinks */
#ifndef FH_PATH_CACHE_SIZE
#define FH_PATH_CACHE_SIZE 64
#endif
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

void
fh_path_cache_add_bypath(const fhandle_t *fh, const char *path)
{
	struct nfs_fh tmp;
	fh_encode(&tmp, fh);
	fh_path_cache_add(&tmp, path);
}

#ifdef __FreeBSD__
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
	if (cfh.fh_fsid.val[0] == fh->fh_fsid.val[0] &&
	    cfh.fh_fsid.val[1] == fh->fh_fsid.val[1] &&
	    cfh.fh_fid.fid_len == fh->fh_fid.fid_len &&
	    memcmp(cfh.fh_fid.fid_data, fh->fh_fid.fid_data, cfh.fh_fid.fid_len) == 0)
	return fh_path_cache[i].path;
	}
	return NULL;
}
#else
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

/*
 * Unified wire format (both platforms):
 *   offset 0: magic (2 bytes)
 *   offset 2: fsid[0] (4 bytes)
 *   offset 6: fsid[1] (4 bytes)
 *   offset 10: platform-specific data (dev/ino on Linux, fid on FreeBSD)
 *
 * This ensures fh_lookup_export can always read fsid at offset 2+6.
 */
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
	v32 = (uint32_t)fh->fh_fsid[0];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	v32 = (uint32_t)(fh->fh_fsid[0] >> 32);
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	v32 = (uint32_t)fh->fh_dev;
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
	uint32_t v32;

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
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid[0] = (uint64_t)v32;
	p = nfh->data + 6;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid[0] |= ((uint64_t)v32) << 32;
	p = nfh->data + 10;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_dev = v32;
	p = nfh->data + 14;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_ino = ((uint64_t)v32) << 32;
	p = nfh->data + 18;
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_ino |= v32;
#endif
	return 0;
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
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (ex->fsid_val[0] == fsid0 && ex->fsid_val[1] == fsid1)
	return ex;
	}
	}

	/* Try path cache (handles from this server session) */
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

	/*
	 * fsid didn't match any export root and path cache didn't help.
	 * Try to resolve the path from the file handle using fhopen
	 * and find an export that covers it.
	 */
	{
	fhandle_t fh;
	char path[512];
	int fd;

	if (fh_decode(nfh, &fh) < 0)
	return NULL;

	fd = PORT_FHOPEN(&fh, O_RDONLY);
	if (fd < 0)
	return NULL;
	if (fchdir(fd) == 0 && getcwd(path, sizeof(path)) != NULL) {
	/* Check if path is under any export */
	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (strncmp(path, ex->path, strlen(ex->path)) == 0) {
	if (path[strlen(ex->path)] == '\0' ||
	    path[strlen(ex->path)] == '/') {
	(void)close(fd);
	return ex;
	}
	}
	}
	}
	(void)close(fd);
	}

	return NULL;
}
