#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "fs.h"
#include "log.h"
#include "port.h"
#include "fh.h"

/*
 * On FreeBSD: use fhstat/fhopen/fhstatfs for file-handle-based operations.
 * On Linux: these syscalls don't exist. We rely on path-based operations
 * from the NFS handlers (which use fh_path_cache to resolve paths).
 * The fs_* functions are stubs on Linux that return ENOSYS.
 * All actual operations are done via path-based syscalls in the handlers.
 */

#ifdef __FreeBSD__

int
fs_getattr(const fhandle_t *fh, struct fs_fattr *attr)
{
	struct stat sb;

	if (PORT_FHSTAT(fh, &sb) < 0)
	return errno;
	memset(attr, 0, sizeof(*attr));
	attr->fsid = (((uint64_t)fh->fh_fsid.val[0]) << 32) |
	    (uint64_t)(uint32_t)fh->fh_fsid.val[1];
	attr->mode = sb.st_mode;
	attr->nlink = sb.st_nlink;
	attr->uid = sb.st_uid;
	attr->gid = sb.st_gid;
	attr->size = sb.st_size;
	attr->used = sb.st_blocks;
	attr->fileid = sb.st_ino;
	if (S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode)) {
	attr->rdev_spec[0] = major(sb.st_rdev);
	attr->rdev_spec[1] = minor(sb.st_rdev);
	}
	attr->atime_sec = sb.PORT_ST_ATIM.tv_sec;
	attr->atime_usec = sb.PORT_ST_ATIM.tv_nsec / 1000;
	attr->mtime_sec = sb.PORT_ST_MTIM.tv_sec;
	attr->mtime_usec = sb.PORT_ST_MTIM.tv_nsec / 1000;
	attr->ctime_sec = sb.PORT_ST_CTIM.tv_sec;
	attr->ctime_usec = sb.PORT_ST_CTIM.tv_nsec / 1000;
	return 0;
}

int
fs_statfs(const fhandle_t *fh, struct statfs *sf)
{
	if (PORT_FHSTATFS(fh, sf) < 0)
	return errno;
	return 0;
}

int
fs_open(const fhandle_t *fh, int flags)
{
	int fd = PORT_FHOPEN(fh, flags);
	if (fd < 0)
	return -errno;
	return fd;
}

ssize_t
fs_read(int fd, void *buf, size_t len, off_t offset)
{
	ssize_t n;

	if (len > (size_t)SSIZE_MAX)
	len = (size_t)SSIZE_MAX;
	n = pread(fd, buf, len, offset);
	if (n < 0)
	return -errno;
	return n;	/* bytes read; 0 means end of file */
}

ssize_t
fs_readlink(const fhandle_t *fh, char *buf, size_t buflen)
{
	int fd;
	ssize_t n;

	fd = PORT_FHOPEN(fh, O_RDONLY);
	if (fd < 0)
	return -errno;
	n = readlinkat(fd, ".", buf, buflen - 1);
	(void)close(fd);
	if (n < 0)
	return -errno;
	buf[n] = '\0';
	return n;
}

int
fs_opendir(const fhandle_t *fh, DIR **dirp)
{
	int fd;

	fd = PORT_FHOPEN(fh, O_RDONLY);
	if (fd < 0) {
	*dirp = NULL;
	return -errno;
	}
	*dirp = fdopendir(fd);
	if (*dirp == NULL) {
	(void)close(fd);
	return -errno;
	}
	return 0;
}

int
fs_readdir(DIR *dirp, uint64_t *inode, char *name, size_t namelen)
{
	struct dirent *de;
	unsigned n;

	de = readdir(dirp);
	if (de == NULL)
	return 1;	/* EOF */
	*inode = de->d_ino;
	n = strlen(de->d_name);
	if (n + 1 > namelen)
	n = (unsigned)(namelen - 1);
	memcpy(name, de->d_name, n);
	name[n] = '\0';
	return 0;
}

int
fs_lookup(const struct export *ex, const char *path,
    fhandle_t *outfh, int *is_symlink)
{
	char rp[REALPATH_BUF_LEN];
	struct stat sb;
	const char *expath = ex->path;
	size_t elen;
	char relpath[MAX_PATH_LEN];
	char curpath[MAX_PATH_LEN];
	char *comp;
	char *save;
	char tmp[MAX_PATH_LEN];
	fhandle_t curfh;

	*is_symlink = 0;

	/* Exact match */
	if (strcmp(path, expath) == 0) {
	if (port_lgetfh(path, outfh) < 0)
	return errno;
	if (lstat(path, &sb) < 0)
	return errno;
	if (S_ISLNK(sb.st_mode))
	*is_symlink = 1;
	return 0;
	}

	/* Must be a subdirectory of the export */
	elen = strlen(expath);
	if (strncmp(path, expath, elen) != 0 || path[elen] != '/') {
	return ENOENT;
	}

	/* Build relative path */
	strlcpy(relpath, path + elen + 1, sizeof(relpath));

	/* Walk the path component by component */
	if (port_lgetfh(expath, &curfh) < 0)
	return errno;
	strlcpy(curpath, expath, sizeof(curpath));

	save = relpath;
	while ((comp = strtok_r(save, "/", &save)) != NULL) {
	if (strcmp(comp, ".") == 0)
	continue;
	if (strcmp(comp, "..") == 0)
	continue;	/* cannot leave export root */

	/* Build full path for this component */
	snprintf(tmp, sizeof(tmp), "%s/%s", curpath, comp);

	if (port_lgetfh(tmp, &curfh) < 0)
	return errno;
	if (lstat(tmp, &sb) < 0)
	return errno;
	if (!S_ISDIR(sb.st_mode))
	return ENOTDIR;
	strlcpy(curpath, tmp, sizeof(curpath));
	}

	if (lstat(curpath, &sb) < 0)
	return errno;
	if (S_ISLNK(sb.st_mode))
	*is_symlink = 1;

	/* Verify the resolved path is on the same filesystem */
	if (realpath(curpath, rp) == NULL)
	return errno;
	if (strcmp(rp, curpath) != 0) {
	/* Path resolution changed things - use realpath result */
	if (port_lgetfh(rp, outfh) < 0)
	return errno;
	} else {
	memcpy(outfh, &curfh, sizeof(*outfh));
	}
	return 0;
}

long
fs_telldir(DIR *dirp)
{
	return telldir(dirp);
}

void
fs_seekdir(DIR *dirp, long offset)
{
	seekdir(dirp, offset);
}

#else /* Linux */

/* Linux: fhandle_t contains dev/fsid/ino. Resolve to path via cache,
 * then use path-based syscalls. */

int
fs_getattr(const fhandle_t *fh, struct fs_fattr *attr)
{
	const char *path;
	struct stat sb;

	path = fh_path_cache_getbyfh(fh);
	if (path == NULL)
	return ENOENT;
	if (stat(path, &sb) < 0)
	return errno;
	memset(attr, 0, sizeof(*attr));
	attr->fsid = (((uint64_t)fh->fh_fsid[0]) << 32) |
	    (uint64_t)(uint32_t)fh->fh_fsid[1];
	attr->mode = sb.st_mode;
	attr->nlink = sb.st_nlink;
	attr->uid = sb.st_uid;
	attr->gid = sb.st_gid;
	attr->size = sb.st_size;
	attr->used = sb.st_blocks;
	attr->fileid = sb.st_ino;
	if (S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode)) {
	attr->rdev_spec[0] = major(sb.st_rdev);
	attr->rdev_spec[1] = minor(sb.st_rdev);
	}
	attr->atime_sec = sb.st_atim.tv_sec;
	attr->atime_usec = sb.st_atim.tv_nsec / 1000;
	attr->mtime_sec = sb.st_mtim.tv_sec;
	attr->mtime_usec = sb.st_mtim.tv_nsec / 1000;
	attr->ctime_sec = sb.st_ctim.tv_sec;
	attr->ctime_usec = sb.st_ctim.tv_nsec / 1000;
	return 0;
}

int
fs_statfs(const fhandle_t *fh, struct statfs *sf)
{
	const char *path;

	path = fh_path_cache_getbyfh(fh);
	if (path == NULL)
	return ENOENT;
	if (statfs(path, sf) < 0)
	return errno;
	return 0;
}

int
fs_open(const fhandle_t *fh, int flags)
{
	const char *path;

	path = fh_path_cache_getbyfh(fh);
	if (path == NULL)
	return -ENOENT;
	return open(path, flags);
}

ssize_t
fs_read(int fd, void *buf, size_t len, off_t offset)
{
	ssize_t n;

	if (len > (size_t)SSIZE_MAX)
	len = (size_t)SSIZE_MAX;
	n = pread(fd, buf, len, offset);
	if (n < 0)
	return -errno;
	return n;	/* bytes read; 0 means end of file */
}

ssize_t
fs_readlink(const fhandle_t *fh, char *buf, size_t buflen)
{
	const char *path;

	path = fh_path_cache_getbyfh(fh);
	if (path == NULL)
	return -ENOENT;
	return readlink(path, buf, buflen);
}

int
fs_opendir(const fhandle_t *fh, DIR **dirp)
{
	const char *path;

	path = fh_path_cache_getbyfh(fh);
	if (path == NULL)
	return ENOENT;
	DIR *d = opendir(path);
	if (d == NULL)
	return errno;
	*dirp = d;
	return 0;
}

int
fs_readdir(DIR *dirp, uint64_t *inode, char *name, size_t namelen)
{
	struct dirent *de;
	unsigned n;

	de = readdir(dirp);
	if (de == NULL)
	return 1;	/* EOF */
	*inode = de->d_ino;
	n = strlen(de->d_name);
	if (n + 1 > namelen)
	n = (unsigned)(namelen - 1);
	memcpy(name, de->d_name, n);
	name[n] = '\0';
	return 0;
}

int
fs_lookup(const struct export *ex, const char *path,
    fhandle_t *outfh, int *is_symlink)
{
	struct stat sb;
	struct statfs sf;

	(void)ex;

	*is_symlink = 0;
	if (lstat(path, &sb) < 0)
	return errno;
	if (statfs(path, &sf) < 0)
	return errno;
	outfh->fh_fsid[0] = (uint64_t)sf.f_fsid.__val[0];
	outfh->fh_fsid[1] = (uint64_t)sf.f_fsid.__val[1];
	outfh->fh_ino = sb.st_ino;
	outfh->fh_dev = sb.st_dev;
	if (S_ISLNK(sb.st_mode))
	*is_symlink = 1;
	return 0;
}

long
fs_telldir(DIR *dirp)
{
	return telldir(dirp);
}

void
fs_seekdir(DIR *dirp, long offset)
{
	seekdir(dirp, offset);
}

#endif /* __FreeBSD__ */

int
fs_setattr_path(const char *path, const struct fs_setattr *sa)
{
	struct stat sb;
	int lnk, err = 0;

	if (lstat(path, &sb) < 0)
		return errno;
	lnk = S_ISLNK(sb.st_mode);

	if (sa->have_size) {
		if (lnk)
			return EINVAL;
		if (truncate(path, (off_t)sa->size) < 0)
			return errno;
	}
	if (sa->have_mode && !lnk) {
		if (chmod(path, (mode_t)(sa->mode & 07777)) < 0)
			return errno;
	}
	if (sa->have_uid || sa->have_gid) {
		if (lchown(path, sa->have_uid ? (uid_t)sa->uid : (uid_t)-1,
		    sa->have_gid ? (gid_t)sa->gid : (gid_t)-1) < 0)
			return errno;
	}
	if (sa->atime_mode != 0 || sa->mtime_mode != 0) {
		struct timeval tv[2], now;

		gettimeofday(&now, NULL);
		tv[0].tv_sec = sb.PORT_ST_ATIM.tv_sec;
		tv[0].tv_usec = sb.PORT_ST_ATIM.tv_nsec / 1000;
		tv[1].tv_sec = sb.PORT_ST_MTIM.tv_sec;
		tv[1].tv_usec = sb.PORT_ST_MTIM.tv_nsec / 1000;
		if (sa->atime_mode == 1)
			tv[0] = now;
		else if (sa->atime_mode == 2) {
			tv[0].tv_sec = (time_t)sa->atime_sec;
			tv[0].tv_usec = sa->atime_usec;
		}
		if (sa->mtime_mode == 1)
			tv[1] = now;
		else if (sa->mtime_mode == 2) {
			tv[1].tv_sec = (time_t)sa->mtime_sec;
			tv[1].tv_usec = sa->mtime_usec;
		}
		if (lutimes(path, tv) < 0)
			err = errno;
	}
	return err;
}

int
fs_unlink(const char *path)
{
	int rc = unlink(path);

	if (rc == 0)
	fh_path_cache_forget(path);
	return rc;
}

int
fs_rmdir(const char *path)
{
	int rc = rmdir(path);

	if (rc == 0)
	fh_path_cache_forget(path);
	return rc;
}

int
fs_rename(const char *from, const char *to)
{
	int rc = rename(from, to);

	if (rc == 0)
	fh_path_cache_rename(from, to);
	return rc;
}

int
fs_access_groups(const struct fs_fattr *attr, uint32_t uid, uint32_t gid,
    const uint32_t *gids, unsigned ngids, int mode)
{
	int want = (mode > 7) ? ((mode >> 6) & 7) : (mode & 7);
	int have;
	unsigned i;

	if (want == 0)
		return 0;
	if (uid == 0) {
		if ((want & 1) && !S_ISDIR(attr->mode) &&
		    (attr->mode & 0111) == 0)
			return -EACCES;
		return 0;
	}
	if (uid == attr->uid) {
		have = (attr->mode >> 6) & 7;
	} else {
		int in_group = (gid == attr->gid);

		for (i = 0; !in_group && i < ngids; i++)
			in_group = (gids[i] == attr->gid);
		have = in_group ? ((attr->mode >> 3) & 7) : (attr->mode & 7);
	}
	return ((have & want) == want) ? 0 : -EACCES;
}

int
fs_access(const struct fs_fattr *attr, uint32_t uid, uint32_t gid, int mode)
{
	return fs_access_groups(attr, uid, gid, NULL, 0, mode);
}
