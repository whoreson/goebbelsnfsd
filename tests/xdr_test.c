#include <stdio.h>
#include <string.h>

#include "xdr.h"

static int fails = 0;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

static void
test_u32(void)
{
	unsigned char b[8];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_u32(&x, 0x01020304u);
	CHECK(xdr_ok(&x) && xdr_pos(&x) == 4);
	CHECK(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4);
	xdr_init(&x, b, 4);
	CHECK(xdr_get_u32(&x) == 0x01020304u);
	CHECK(xdr_ok(&x) && xdr_left(&x) == 0);
}

static void
test_u64(void)
{
	unsigned char b[8];
	struct xdr x;
	uint64_t v = ((uint64_t)0x01020304u << 32) | 0x05060708u;

	xdr_init(&x, b, sizeof(b));
	xdr_put_u64(&x, v);
	CHECK(b[0] == 1 && b[7] == 8);
	xdr_init(&x, b, sizeof(b));
	CHECK(xdr_get_u64(&x) == v);
	CHECK(xdr_ok(&x));
}

static void
test_bool(void)
{
	unsigned char b[4];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_u32(&x, 2);
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_bool(&x);
	CHECK(!xdr_ok(&x));
}

static void
test_var(void)
{
	unsigned char b[64];
	unsigned char out[16];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_var(&x, "abc", 3);
	CHECK(xdr_ok(&x) && xdr_pos(&x) == 8);
	CHECK(b[7] == 0);	/* padding is zero */
	xdr_init(&x, b, sizeof(b));
	CHECK(xdr_get_var(&x, out, sizeof(out)) == 3);
	CHECK(memcmp(out, "abc", 3) == 0);
	CHECK(xdr_ok(&x) && xdr_pos(&x) == 8);

	/* length above maxlen */
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_var(&x, out, 2);
	CHECK(!xdr_ok(&x));
}

static void
test_hostile_length(void)
{
	unsigned char b[8];
	unsigned char out[256];
	struct xdr x;

	/* length 0xFFFFFFFF */
	b[0] = b[1] = b[2] = b[3] = 0xFF;
	memset(b + 4, 0, 4);
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_var(&x, out, sizeof(out));
	CHECK(!xdr_ok(&x));

	/* length 100, but only 4 bytes follow, maxlen allows it */
	memset(b, 0, sizeof(b));
	b[3] = 100;
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_var(&x, out, sizeof(out));
	CHECK(!xdr_ok(&x));
}

static void
test_overrun_sticky(void)
{
	unsigned char b[6];
	struct xdr x;

	memset(b, 0, sizeof(b));
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_u32(&x);
	CHECK(xdr_ok(&x));
	CHECK(xdr_get_u32(&x) == 0);	/* only 2 bytes left */
	CHECK(!xdr_ok(&x));
	CHECK(xdr_get_u32(&x) == 0);
	CHECK(!xdr_ok(&x));	/* error stays set */
}

static void
test_put_overflow(void)
{
	unsigned char b[4];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_u64(&x, 1);
	CHECK(!xdr_ok(&x));
}

static void
test_string(void)
{
	unsigned char b[64];
	char out[16];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_string(&x, "hello");
	xdr_init(&x, b, sizeof(b));
	CHECK(xdr_get_string(&x, out, 15) == 5);
	CHECK(strcmp(out, "hello") == 0);
	CHECK(xdr_ok(&x));

	/* embedded NUL */
	xdr_init(&x, b, sizeof(b));
	xdr_put_var(&x, "a\0b", 3);
	xdr_init(&x, b, sizeof(b));
	(void)xdr_get_string(&x, out, 15);
	CHECK(!xdr_ok(&x));
}

static void
test_fixed(void)
{
	unsigned char b[16];
	unsigned char out[5];
	struct xdr x;

	memset(b, 0xAA, sizeof(b));
	xdr_init(&x, b, sizeof(b));
	xdr_put_fixed(&x, "12345", 5);
	CHECK(xdr_pos(&x) == 8);
	CHECK(b[5] == 0 && b[6] == 0 && b[7] == 0);
	xdr_init(&x, b, sizeof(b));
	xdr_get_fixed(&x, out, 5);
	CHECK(xdr_ok(&x) && memcmp(out, "12345", 5) == 0);
}

static void
test_patch(void)
{
	unsigned char b[8];
	struct xdr x;

	xdr_init(&x, b, sizeof(b));
	xdr_put_u32(&x, 0);
	xdr_put_u32(&x, 5);
	xdr_patch_u32(&x, 0, 9);
	CHECK(xdr_ok(&x));
	xdr_init(&x, b, sizeof(b));
	CHECK(xdr_get_u32(&x) == 9);
	CHECK(xdr_get_u32(&x) == 5);

	xdr_init(&x, b, sizeof(b));
	xdr_put_u32(&x, 0);
	xdr_patch_u32(&x, 4, 1);	/* beyond written data */
	CHECK(!xdr_ok(&x));
}

int
main(void)
{
	test_u32();
	test_u64();
	test_bool();
	test_var();
	test_hostile_length();
	test_overrun_sticky();
	test_put_overflow();
	test_string();
	test_fixed();
	test_patch();
	if (fails == 0)
	printf("xdr_test: all passed\n");
	return fails != 0;
}
