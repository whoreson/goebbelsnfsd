#ifndef DIRCACHE_H
#define DIRCACHE_H

/*
 * Directory listings for READDIR (NFSv2) and READDIR/READDIRPLUS (NFSv3).
 *
 * See dirlist.h: cookies come from the entry names, so a continuation
 * request is right even when the listing is read again.
 *
 * A request with cookie 0 always reads the real directory. A later
 * request may reuse the last listing for a short time, but only if the
 * directory did not change (same mtime and ctime). An old listing can
 * show an entry too long or too late; it can never hide the rest of a
 * directory.
 */

#include "dirlist.h"
#include "fh.h"
#include "fs.h"

/*
 * Get the listing of directory "nfh" (decoded as "fh", attributes "attr")
 * for a request with this cookie. "bits" is DL_BITS_V2 or DL_BITS_V3.
 * The result is valid until the next call. Return NULL on error.
 */
const struct dirlist *dircache_get(const struct nfs_fh *nfh,
    const fhandle_t *fh, const struct fs_fattr *attr, uint64_t cookie,
    unsigned bits);

#endif
