#include <sys/mount.h>
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

int
fs_getattr(const fhandle_t *fh, struct fs_fattr *attr)
{
	struct stat sb;

	if (fhstat(fh, &sb) < 0)
	return errno;
	memset(attr, 0, sizeof(*attr));
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
	/* atime/mtime/ctime: struct timespec on FreeBSD 8 */
	attr->atime_sec = sb.st_atimespec.tv_sec;
	attr->atime_usec = sb.st_atimespec.tv_nsec / 1000;
	attr->mtime_sec = sb.st_mtimespec.tv_sec;
	attr->mtime_usec = sb.st_mtimespec.tv_nsec / 1000;
	attr->ctime_sec = sb.st_ctimespec.tv_sec;
	attr->ctime_usec = sb.st_ctimespec.tv_nsec / 1000;
	return 0;
}

int
fs_statfs(const fhandle_t *fh, struct statfs *sf)
{
	if (fhstatfs(fh, sf) < 0)
	return errno;
	return 0;
}

int
fs_open(const fhandle_t *fh, int flags)
{
	int fd = fhopen(fh, flags);
	if (fd < 0)
	return -errno;
	return fd;
}

ssize_t
fs_read(int fd, void *buf, size_t len, off_t offset)
{
	off_t off;

	off = lseek(fd, offset, SEEK_SET);
	if (off < 0)
	return -errno;
	if ((ssize_t)len > SSIZE_MAX)
	len = SSIZE_MAX;
	if (read(fd, buf, len) < 0)
	return -errno;
	return 0;
}

ssize_t
fs_readlink(const fhandle_t *fh, char *buf, size_t buflen)
{
	int fd;
	ssize_t n;

	fd = fhopen(fh, O_RDONLY);
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

	fd = fhopen(fh, O_RDONLY);
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
	char rp[MAX_PATH_LEN];
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
	if (lgetfh(path, outfh) < 0)
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
	strcpy(relpath, path + elen + 1);

	/* Walk the path component by component */
	if (lgetfh(expath, &curfh) < 0)
	return errno;
	strcpy(curpath, expath);

	save = relpath;
	while ((comp = strtok_r(save, "/", &save)) != NULL) {
	if (strcmp(comp, ".") == 0)
	continue;
	if (strcmp(comp, "..") == 0)
	continue;	/* cannot leave export root */

	/* Build full path for this component */
	snprintf(tmp, sizeof(tmp), "%s/%s", curpath, comp);

	if (lgetfh(tmp, &curfh) < 0)
	return errno;
	if (lstat(tmp, &sb) < 0)
	return errno;
	if (!S_ISDIR(sb.st_mode))
	return ENOTDIR;
	strcpy(curpath, tmp);
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
	if (lgetfh(rp, outfh) < 0)
	return errno;
	} else {
	memcpy(outfh, &curfh, sizeof(*outfh));
	}
	return 0;
}

int
fs_access(const struct fs_fattr *attr, uint32_t uid, uint32_t gid,
    int mode)
{
	uint32_t m = attr->mode & 0777;

	/* Root bypasses checks */
	if (uid == 0)
	return 0;

	/* Owner */
	if (uid == attr->uid) {
	if (mode & 0400 && !(m & 0400)) return -EACCES;
	if (mode & 0200 && !(m & 0200)) return -EACCES;
	if (mode & 0100 && !(m & 0100)) return -EACCES;
	return 0;
	}

	/* Group */
	if (gid == attr->gid) {
	if (mode & 0400 && !(m & 0040)) return -EACCES;
	if (mode & 0200 && !(m & 0020)) return -EACCES;
	if (mode & 0100 && !(m & 0010)) return -EACCES;
	return 0;
	}

	/* Other */
	if (mode & 0400 && !(m & 0004)) return -EACCES;
	if (mode & 0200 && !(m & 0002)) return -EACCES;
	if (mode & 0100 && !(m & 0001)) return -EACCES;
	return 0;
}
