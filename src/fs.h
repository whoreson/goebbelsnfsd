#ifndef FS_H
#define FS_H

#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>

#include "fh.h"
#include "port.h"

/* File attributes for NFS reply */
struct fs_fattr {
	uint32_t mode;
	uint64_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint64_t used;
	uint32_t rdev_spec[2];
	uint64_t fsid;
	uint64_t fileid;
	uint64_t atime_sec;
	uint64_t atime_usec;
	uint64_t mtime_sec;
	uint64_t mtime_usec;
	uint64_t ctime_sec;
	uint64_t ctime_usec;
};

/* Directory entry for READDIR */
struct fs_dirent {
	uint64_t inode;
	uint32_t namelen;
	char     name[256];
};

/* Get attributes for a file handle. Returns 0 on success, errno on failure. */
int  fs_getattr(const fhandle_t *fh, struct fs_fattr *attr);

/* Get filesystem stats. Returns 0 on success, errno on failure. */
int  fs_statfs(const fhandle_t *fh, struct statfs *sf);

/* Open a file handle for I/O. Returns fd >= 0, or -errno. */
int  fs_open(const fhandle_t *fh, int flags);

/* Read from an open fd. Returns bytes read, or -errno. */
ssize_t fs_read(int fd, void *buf, size_t len, off_t offset);

/* Read a symlink target. Returns bytes written (excluding NUL), or -errno. */
ssize_t fs_readlink(const fhandle_t *fh, char *buf, size_t buflen);

/* Open directory and return fd + DIR*. Returns 0 on success. */
int  fs_opendir(const fhandle_t *fh, DIR **dirp);

/* Read next directory entry. Returns 0 on success, 1 on EOF, -errno. */
int  fs_readdir(DIR *dirp, uint64_t *inode, char *name, size_t namelen);

/* Resolve a path within an export. Returns 0 on success, errno on failure.
 * Sets *outfh on success. *is_symlink is set if the result is a symlink. */
int  fs_lookup(const struct export *ex, const char *path,
    fhandle_t *outfh, int *is_symlink);

/* Check UNIX permissions. Returns 0 if allowed, -EACCES if denied. */
int  fs_access(const struct fs_fattr *attr, uint32_t uid, uint32_t gid,
    int mode);

#endif
