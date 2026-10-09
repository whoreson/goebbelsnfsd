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

/* Get current directory offset (for stable cookies). */
long fs_telldir(DIR *dirp);

/* Seek to directory offset. */
void fs_seekdir(DIR *dirp, long offset);

/* Resolve a path within an export. Returns 0 on success, errno on failure.
 * Sets *outfh on success. *is_symlink is set if the result is a symlink. */
int  fs_lookup(const struct export *ex, const char *path,
    fhandle_t *outfh, int *is_symlink);

/* Attribute changes for SETATTR. Time mode: 0 = keep, 1 = server time, 2 = set. */
struct fs_setattr {
	int       have_mode, have_uid, have_gid, have_size;
	uint32_t  mode, uid, gid;
	uint64_t  size;
	int       atime_mode, mtime_mode;
	int64_t   atime_sec, mtime_sec;
	int32_t   atime_usec, mtime_usec;
};

/*
 * Apply the changes to "path". A symlink is never followed: the link is
 * changed (owner, times) or the change is refused (size: EINVAL), and
 * mode is skipped for a link. Return 0, or the first errno.
 */
int  fs_setattr_path(const char *path, const struct fs_setattr *sa);

/*
 * Namespace changes. Same result as the system call (0 or -1 with errno).
 * On success the path cache is updated, so no handle keeps an old path.
 */
int  fs_unlink(const char *path);
int  fs_rmdir(const char *path);
int  fs_rename(const char *from, const char *to);

/*
 * Check UNIX permissions for a (mapped) user.
 * "mode" is 0..7 (r=4 w=2 x=1) or the owner form 0400/0200/0100.
 * Return 0 if allowed, -EACCES if denied.
 * Root (uid 0) may read and write anything; it may execute only a
 * directory or a file that has an execute bit.
 */
int  fs_access(const struct fs_fattr *attr, uint32_t uid, uint32_t gid,
    int mode);
/* Same, with the supplementary groups of the caller. */
int  fs_access_groups(const struct fs_fattr *attr, uint32_t uid,
    uint32_t gid, const uint32_t *gids, unsigned ngids, int mode);

#endif
