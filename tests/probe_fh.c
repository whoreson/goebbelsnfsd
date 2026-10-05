#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
probe(const char *path)
{
	fhandle_t fh;
	struct stat a, b;
	struct statfs sf;
	int fd;

	printf("== %s\n", path);
	if (lstat(path, &a) < 0) {
	printf("lstat: %s\n", strerror(errno));
	return;
	}
	if (lgetfh(path, &fh) < 0) {
	printf("lgetfh: FAIL %s\n", strerror(errno));
	return;
	}
	printf("lgetfh: ok, fsid=%08lx:%08lx fid_len=%u sizeof(fhandle_t)=%u\n",
	    (unsigned long)fh.fh_fsid.val[0], (unsigned long)fh.fh_fsid.val[1],
	    (unsigned)fh.fh_fid.fid_len, (unsigned)sizeof(fh));
	if (fhstat(&fh, &b) < 0)
	printf("fhstat: FAIL %s\n", strerror(errno));
	else
	printf("fhstat: ok, ino match=%d mode match=%d\n",
	    a.st_ino == b.st_ino, a.st_mode == b.st_mode);
	if (fhstatfs(&fh, &sf) < 0)
	printf("fhstatfs: FAIL %s\n", strerror(errno));
	else
	printf("fhstatfs: ok, fstype=%s\n", sf.f_fstypename);
	fd = fhopen(&fh, O_RDONLY);
	if (fd < 0)
	printf("fhopen: FAIL %s\n", strerror(errno));
	else {
	printf("fhopen: ok\n");
	(void)close(fd);
	}
}

int
main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
	probe(argv[i]);
	return 0;
}
