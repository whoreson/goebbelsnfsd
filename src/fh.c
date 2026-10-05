#include <string.h>
#include <sys/mount.h>

#include "fh.h"
#include "conf.h"

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
	/* fid_len (2 bytes, big-endian) */
	v16 = (uint16_t)fh->fh_fid.fid_len;
	p[0] = (v16 >> 8) & 0xFF;
	p[1] = v16 & 0xFF;
	p += 2;
	/* fsid[0] (4 bytes, big-endian) */
	v32 = fh->fh_fsid.val[0];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	/* fsid[1] (4 bytes, big-endian) */
	v32 = fh->fh_fsid.val[1];
	p[0] = (v32 >> 24) & 0xFF;
	p[1] = (v32 >> 16) & 0xFF;
	p[2] = (v32 >> 8) & 0xFF;
	p[3] = v32 & 0xFF;
	p += 4;
	/* fid (20 bytes) */
	memcpy(p, fh->fh_fid.fid_data, fh->fh_fid.fid_len);
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
	if (fid_len == 0 || fid_len > 20)
	return 0;
	return 1;
}

int
fh_decode(const struct nfs_fh *nfh, fhandle_t *fh)
{
	const uint8_t *p = nfh->data + 4; /* skip magic + fid_len */
	uint32_t v32;

	memset(fh, 0, sizeof(*fh));
	/* fsid[0] */
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[0] = v32;
	p += 4;
	/* fsid[1] */
	v32 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fh->fh_fsid.val[1] = v32;
	p += 4;
	/* fid */
	fh->fh_fid.fid_len = (u_short)(((uint16_t)nfh->data[2] << 8) | nfh->data[3]);
	memcpy(fh->fh_fid.fid_data, p, fh->fh_fid.fid_len);
	return 0;
}

const struct export *
fh_lookup_export(const struct nfs_fh *nfh)
{
	const uint8_t *p = nfh->data + 4;
	unsigned i;
	const struct export *ex;
	uint32_t fsid0, fsid1;

	fsid0 = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	fsid1 = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
	    ((uint32_t)p[6] << 8) | (uint32_t)p[7];

	for (i = 0; i < conf_export_count(); i++) {
	ex = conf_get_export(i);
	if (ex->fsid.val[0] == fsid0 && ex->fsid.val[1] == fsid1)
	return ex;
	}
	return NULL;
}
