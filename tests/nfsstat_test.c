/* Check nfsstat values against RFC 1094 / RFC 1813 and the errno map. */
#include "port.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "nfs_common.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

int
main(void)
{
	/* Wire values fixed by the RFCs. */
	CHECK(NFSERR_INVAL == 22);
	CHECK(NFSERR_FBIG == 27);
	CHECK(NFSERR_NOSPC == 28);
	CHECK(NFSERR_ROFS == 30);
	CHECK(NFSERR_NAMETOOLONG == 63);
	CHECK(NFSERR_NOTEMPTY == 66);
	CHECK(NFSERR_DQUOT == 69);
	CHECK(NFSERR_STALE == 70);
	CHECK(NFSERR_NOT_SYNC == 10002);

	/* errno mapping */
	CHECK(nfs_errno(0) == NFS_OK);
	CHECK(nfs_errno(ENOENT) == NFSERR_NOENT);
	CHECK(nfs_errno(ENOSPC) == NFSERR_NOSPC);
	CHECK(nfs_errno(EFBIG) == NFSERR_FBIG);
	CHECK(nfs_errno(EROFS) == NFSERR_ROFS);
	CHECK(nfs_errno(ENOTEMPTY) == NFSERR_NOTEMPTY);
	CHECK(nfs_errno(EDQUOT) == NFSERR_DQUOT);
	CHECK(nfs_errno(ESTALE) == NFSERR_STALE);
	CHECK(nfs_errno(EINVAL) == NFSERR_INVAL);
	/* An unknown errno must not look like a permission error. */
	CHECK(nfs_errno(EBUSY) == NFSERR_IO);

	/* File name validation: the path traversal guard. */
	CHECK(nfs_name_ok("file", 0));
	CHECK(nfs_name_ok("a..b", 0));
	CHECK(nfs_name_ok(".hidden", 0));
	CHECK(!nfs_name_ok("", 0));
	CHECK(!nfs_name_ok("a/b", 0));
	CHECK(!nfs_name_ok("../x", 1));
	CHECK(!nfs_name_ok("/etc", 1));
	CHECK(!nfs_name_ok(".", 0));
	CHECK(!nfs_name_ok("..", 0));
	CHECK(nfs_name_ok(".", 1));
	CHECK(nfs_name_ok("..", 1));
	{
		/* XDR string "../x" must make the decoder fail. */
		unsigned char b[16] = { 0,0,0,4, '.','.','/','x' };
		char out[32];
		struct xdr x;

		xdr_init(&x, b, 8);
		CHECK(nfs_get_name(&x, out, sizeof(out) - 1, 1) == 0);
		CHECK(!xdr_ok(&x));

		memcpy(b, "\0\0\0\4name", 8);
		xdr_init(&x, b, 8);
		CHECK(nfs_get_name(&x, out, sizeof(out) - 1, 0) == 4);
		CHECK(strcmp(out, "name") == 0 && xdr_ok(&x));
	}

	/* strlcpy / strlcat return the full length, so cuts can be seen */
	{
		char b[8];

		CHECK(strlcpy(b, "abc", sizeof(b)) == 3 && strcmp(b, "abc") == 0);
		CHECK(strlcpy(b, "0123456789", sizeof(b)) == 10);
		CHECK(strcmp(b, "0123456") == 0);
		CHECK(strlcpy(b, "abc", 0) == 3);
		strcpy(b, "ab");
		CHECK(strlcat(b, "cd", sizeof(b)) == 4 && strcmp(b, "abcd") == 0);
		CHECK(strlcat(b, "efghij", sizeof(b)) == 10);
		CHECK(strcmp(b, "abcdefg") == 0);
	}

	if (fails == 0)
		printf("nfsstat_test: OK\n");
	return fails != 0;
}
