/*
 * port.h - Cross-platform compatibility layer (FreeBSD 8 / Linux)
 *
 * MUST be included before any system headers to ensure _BSD_SOURCE
 * and _DEFAULT_SOURCE are defined early enough on Linux glibc.
 */

#ifndef PORT_H
#define PORT_H

#ifndef __FreeBSD__
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>

/* NULL may be needed before stddef.h is included */
#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef __FreeBSD__

#include <sys/mount.h>

#define PORT_FHOPEN(fh, flags)	fhopen((fh), (flags))
#define PORT_FHSTAT(fh, sb)	fhstat((fh), (sb))
#define PORT_FHSTATFS(fh, sf)	fhstatfs((fh), (sf))

static inline int
port_lgetfh(const char *path, fhandle_t *fhp)
{
	return lgetfh(path, fhp);
}

#define PORT_FSID_VAL0(sf)	((sf).f_fsid.val[0])
#define PORT_FSID_VAL1(sf)	((sf).f_fsid.val[1])
#define PORT_ST_ATIM	st_atimespec
#define PORT_ST_MTIM	st_mtimespec
#define PORT_ST_CTIM	st_ctimespec

#ifndef MAXFIDSZ
#define MAXFIDSZ	16
#endif

#else /* Linux */

#include <sys/statfs.h>

struct linux_fhandle {
	uint32_t fh_fsid[2];	/* fsid (2 x 32-bit) */
	uint64_t fh_ino;	/* inode */
	uint32_t fh_dev;	/* st_dev (major:minor packed) */
};
typedef struct linux_fhandle fhandle_t;

#define MAXFIDSZ	16

/* Linux: no fhopen, fhstat, fhstatfs */
#define PORT_FHOPEN(fh, flags)	(-1)
#define PORT_FHSTAT(fh, sb)	(-1)
#define PORT_FHSTATFS(fh, sf)	(-1)

#define PORT_FSID_VAL0(sf)	((sf).f_fsid.__val[0])
#define PORT_FSID_VAL1(sf)	((sf).f_fsid.__val[1])
#define PORT_ST_ATIM	st_atim
#define PORT_ST_MTIM	st_mtim
#define PORT_ST_CTIM	st_ctim

static inline int
port_lgetfh(const char *path, fhandle_t *fhp)
{
	struct stat st;
	struct statfs sf;
	if (stat(path, &st) < 0)
	{
	 return -1;
	}
	if (statfs(path, &sf) < 0)
	{
	 return -1;
	}
	if (fhp != NULL)
	{
	 fhp->fh_fsid[0] = (uint32_t)PORT_FSID_VAL0(sf);
	 fhp->fh_fsid[1] = (uint32_t)PORT_FSID_VAL1(sf);
	 fhp->fh_ino = st.st_ino;
	 fhp->fh_dev = (uint32_t)st.st_dev;
	}
	return 0;
}

#endif /* __FreeBSD__ */

/* BSD strlcpy - not available on old Linux/Digital UNIX */
#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

/* BSD strlcat - not available on old Linux/Digital UNIX */
#ifndef HAVE_STRLCAT
size_t strlcat(char *dst, const char *src, size_t siz);
#endif

#endif /* PORT_H */
