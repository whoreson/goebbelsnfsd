/* fs_setattr_path must not follow symlinks. Linux build only. */
#include "port.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fs.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

int
main(void)
{
#ifdef __FreeBSD__
	return 0;
#else
	char dir[] = "/tmp/satesttXXXXXX";
	char outside[300], link[300], reg[300], cmd[400];
	struct fs_setattr sa;
	struct stat sb;
	int fd;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(outside, sizeof(outside), "%s/outside", dir);
	snprintf(link, sizeof(link), "%s/link", dir);
	snprintf(reg, sizeof(reg), "%s/reg", dir);
	fd = open(outside, O_CREAT | O_WRONLY, 0640);
	CHECK(write(fd, "0123456789", 10) == 10);
	close(fd);
	fd = open(reg, O_CREAT | O_WRONLY, 0640);
	CHECK(write(fd, "0123456789", 10) == 10);
	close(fd);
	CHECK(symlink(outside, link) == 0);

	/* mode on a link: skipped, target keeps its mode */
	memset(&sa, 0, sizeof(sa));
	sa.have_mode = 1; sa.mode = 0777;
	CHECK(fs_setattr_path(link, &sa) == 0);
	CHECK(stat(outside, &sb) == 0 && (sb.st_mode & 07777) == 0640);

	/* size on a link: refused, target keeps its size */
	memset(&sa, 0, sizeof(sa));
	sa.have_size = 1; sa.size = 0;
	CHECK(fs_setattr_path(link, &sa) == EINVAL);
	CHECK(stat(outside, &sb) == 0 && sb.st_size == 10);

	/* times on a link: the link changes, the target does not */
	memset(&sa, 0, sizeof(sa));
	sa.mtime_mode = 2; sa.mtime_sec = 1000000000;
	CHECK(fs_setattr_path(link, &sa) == 0);
	CHECK(lstat(link, &sb) == 0 && sb.st_mtime == 1000000000);
	CHECK(stat(outside, &sb) == 0 && sb.st_mtime != 1000000000);

	/* regular file: all changes work */
	memset(&sa, 0, sizeof(sa));
	sa.have_mode = 1; sa.mode = 0600;
	sa.have_size = 1; sa.size = 4;
	sa.atime_mode = 2; sa.atime_sec = 1100000000;
	sa.mtime_mode = 2; sa.mtime_sec = 1200000000;
	CHECK(fs_setattr_path(reg, &sa) == 0);
	CHECK(stat(reg, &sb) == 0);
	CHECK((sb.st_mode & 07777) == 0600);
	CHECK(sb.st_size == 4);
	CHECK(sb.st_atime == 1100000000 && sb.st_mtime == 1200000000);

	/* only mtime set: atime is kept */
	memset(&sa, 0, sizeof(sa));
	sa.mtime_mode = 2; sa.mtime_sec = 1300000000;
	CHECK(fs_setattr_path(reg, &sa) == 0);
	CHECK(stat(reg, &sb) == 0 && sb.st_atime == 1100000000);

	/* a missing path gives ENOENT */
	CHECK(fs_setattr_path("/tmp/does/not/exist", &sa) == ENOENT);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	(void)system(cmd);
	if (fails == 0)
		printf("setattr_test: OK\n");
	return fails != 0;
#endif
}
