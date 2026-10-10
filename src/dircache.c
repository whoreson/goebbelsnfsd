#include "port.h"
#include <string.h>
#include <time.h>

#include "dircache.h"

#define SLOTS	16
#define TTL	0	/* 0 = disabled; always read the real directory */

static struct {
	struct nfs_fh	fh;
	unsigned	bits;
	struct dirlist	list;
	time_t		when;
	int64_t		msec, musec, csec, cusec;
	int		valid;
} cache[SLOTS];
static int next_slot;

static int
load(const fhandle_t *fh, struct dirlist *l, unsigned bits)
{
	DIR *dirp;
	uint64_t inode;
	char name[DL_NAMEMAX];

	dl_init(l, bits);
	if (fs_opendir(fh, &dirp) < 0)
		return -1;
	while (fs_readdir(dirp, &inode, name, sizeof(name)) == 0) {
		if (dl_add(l, inode, name) < 0) {
			(void)closedir(dirp);
			dl_free(l);
			return -1;
		}
	}
	(void)closedir(dirp);
	dl_finish(l);
	return 0;
}

const struct dirlist *
dircache_get(const struct nfs_fh *nfh, const fhandle_t *fh,
    const struct fs_fattr *attr, uint64_t cookie, unsigned bits)
{
	time_t now = time(NULL);
	int i, slot = -1;

	for (i = 0; i < SLOTS; i++)
		if (cache[i].valid && cache[i].bits == bits &&
		    memcmp(&cache[i].fh, nfh, sizeof(*nfh)) == 0) {
			slot = i;
			break;
		}
	if (slot >= 0 && cookie != 0 &&
	    now - cache[slot].when <= TTL &&
	    cache[slot].msec == (int64_t)attr->mtime_sec &&
	    cache[slot].musec == (int64_t)attr->mtime_usec &&
	    cache[slot].csec == (int64_t)attr->ctime_sec &&
	    cache[slot].cusec == (int64_t)attr->ctime_usec)
		return &cache[slot].list;

	if (slot < 0) {
		slot = next_slot;
		next_slot = (next_slot + 1) % SLOTS;
	}
	if (cache[slot].valid)
		dl_free(&cache[slot].list);
	cache[slot].valid = 0;
	if (load(fh, &cache[slot].list, bits) < 0)
		return NULL;
	cache[slot].fh = *nfh;
	cache[slot].bits = bits;
	cache[slot].when = now;
	cache[slot].msec = (int64_t)attr->mtime_sec;
	cache[slot].musec = (int64_t)attr->mtime_usec;
	cache[slot].csec = (int64_t)attr->ctime_sec;
	cache[slot].cusec = (int64_t)attr->ctime_usec;
	cache[slot].valid = 1;
	return &cache[slot].list;
}
