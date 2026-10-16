/*
 * inx_test - symlink index: build, load, lookup, corrupt files.
 */
#include "port.h"
#include "inx.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	failures++; } } while (0)

static void
run(const char *cmd)
{
	if (system(cmd) != 0) {
		fprintf(stderr, "command failed: %s\n", cmd);
		exit(1);
	}
}

int
main(void)
{
	char root[] = "/tmp/inx_test.XXXXXX";
	char cmd[1024], idx[256], p[512];
	const char *roots[2];
	const char *got;
	struct stat sb;
	FILE *fp;
	int n, i;

	if (mkdtemp(root) == NULL)
		return 1;
	snprintf(idx, sizeof(idx), "%s/index.bin", root);
	snprintf(cmd, sizeof(cmd),
	    "mkdir -p %s/data/a/b/c %s/data/d && touch %s/data/file && "
	    "ln -s file %s/data/l1 && ln -s ../x %s/data/a/b/c/l2 && "
	    "ln %s/data/file %s/data/hard && ln -s /etc %s/data/d/l3 && "
	    "ln -s l1 %s/data/a/l4",
	    root, root, root, root, root, root, root, root, root);
	run(cmd);

	roots[0] = p;
	snprintf(p, sizeof(p), "%s/data", root);
	/* Same root twice: duplicates must be dropped. */
	roots[1] = p;
	CHECK(inx_build(roots, 2, idx) == 0);
	n = inx_preload(idx);
	CHECK(n == 4);

	{
		static const char *rel[] = { "l1", "a/b/c/l2", "d/l3", "a/l4" };

		for (i = 0; i < 4; i++) {
			snprintf(p, sizeof(p), "%s/data/%s", root, rel[i]);
			CHECK(lstat(p, &sb) == 0);
			got = inx_lookup((uint64_t)sb.st_dev, (uint64_t)sb.st_ino);
			CHECK(got != NULL && strcmp(got, p) == 0);
		}
	}
	/* Regular file and hard link are not in the index. */
	snprintf(p, sizeof(p), "%s/data/file", root);
	CHECK(lstat(p, &sb) == 0);
	CHECK(inx_lookup((uint64_t)sb.st_dev, (uint64_t)sb.st_ino) == NULL);
	/* Misses: wrong ino, wrong dev, zero. */
	CHECK(inx_lookup((uint64_t)sb.st_dev, 0) == NULL);
	CHECK(inx_lookup(0, 1) == NULL);
	CHECK(inx_lookup(~(uint64_t)0, ~(uint64_t)0) == NULL);

	/* A rebuild with no usable root must not replace a good index. */
	roots[0] = "/nonexistent-dir-xyz";
	CHECK(inx_build(roots, 1, idx) == -1);
	CHECK(inx_preload(idx) == 4);

	/* Truncated file: rejected, old mapping stays usable. */
	roots[0] = p;
	snprintf(p, sizeof(p), "%s/data", root);
	CHECK(inx_build(roots, 1, idx) == 0);
	CHECK(inx_preload(idx) == 4);
	snprintf(cmd, sizeof(cmd), "cp %s %s/bad.bin && truncate -s -3 %s/bad.bin",
	    idx, root, root);
	run(cmd);
	snprintf(p, sizeof(p), "%s/bad.bin", root);
	CHECK(inx_preload(p) == -1);
	snprintf(p, sizeof(p), "%s/data/l1", root);
	CHECK(lstat(p, &sb) == 0);
	CHECK(inx_lookup((uint64_t)sb.st_dev, (uint64_t)sb.st_ino) != NULL);

	/* Old format (wrong magic) is rejected. */
	snprintf(p, sizeof(p), "%s/v1.bin", root);
	fp = fopen(p, "wb");
	if (fp != NULL) {
		static const char junk[64] = "GNFHDX\0\1";
		fwrite(junk, sizeof(junk), 1, fp);
		fclose(fp);
	}
	CHECK(inx_preload(p) == -1);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	run(cmd);
	if (failures == 0)
		printf("inx_test: ok\n");
	return failures != 0;
}
