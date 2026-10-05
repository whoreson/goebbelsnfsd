#include <string.h>

#include "xdr.h"

void
xdr_init(struct xdr *x, void *buf, size_t len)
{
	x->base = buf;
	x->len = len;
	x->pos = 0;
	x->err = 0;
}

int
xdr_ok(const struct xdr *x)
{
	return !x->err;
}

size_t
xdr_pos(const struct xdr *x)
{
	return x->pos;
}

size_t
xdr_left(const struct xdr *x)
{
	return x->len - x->pos;
}

/* Advance by n bytes. Return pointer to the start, or NULL on overrun. */
static unsigned char *
xdr_adv(struct xdr *x, size_t n)
{
	unsigned char *p;

	if (x->err || n > x->len - x->pos) {
	x->err = 1;
	return NULL;
	}
	p = x->base + x->pos;
	x->pos += n;
	return p;
}

uint32_t
xdr_get_u32(struct xdr *x)
{
	const unsigned char *p = xdr_adv(x, 4);

	if (p == NULL)
	return 0;
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t
xdr_get_u64(struct xdr *x)
{
	uint64_t hi = xdr_get_u32(x);
	uint64_t lo = xdr_get_u32(x);

	return (hi << 32) | lo;
}

int
xdr_get_bool(struct xdr *x)
{
	uint32_t v = xdr_get_u32(x);

	if (v > 1)
	x->err = 1;
	return v == 1;
}

void
xdr_get_fixed(struct xdr *x, void *out, size_t n)
{
	const unsigned char *p = xdr_adv(x, XDR_PAD(n));

	if (p != NULL)
	memcpy(out, p, n);
}

size_t
xdr_get_var(struct xdr *x, void *out, size_t maxlen)
{
	uint32_t n = xdr_get_u32(x);
	const unsigned char *p;

	if (x->err)
	return 0;
	if (n > maxlen) {
	x->err = 1;
	return 0;
	}
	p = xdr_adv(x, XDR_PAD(n));
	if (p == NULL)
	return 0;
	memcpy(out, p, n);
	return n;
}

size_t
xdr_get_string(struct xdr *x, char *out, size_t maxlen)
{
	size_t n = xdr_get_var(x, out, maxlen);

	if (x->err)
	return 0;
	if (memchr(out, 0, n) != NULL) {
	x->err = 1;
	return 0;
	}
	out[n] = '\0';
	return n;
}

void
xdr_skip_var(struct xdr *x, size_t maxlen)
{
	uint32_t n = xdr_get_u32(x);

	if (x->err)
	return;
	if (n > maxlen) {
	x->err = 1;
	return;
	}
	(void)xdr_adv(x, XDR_PAD(n));
}

void
xdr_put_u32(struct xdr *x, uint32_t v)
{
	unsigned char *p = xdr_adv(x, 4);

	if (p == NULL)
	return;
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

void
xdr_put_u64(struct xdr *x, uint64_t v)
{
	xdr_put_u32(x, (uint32_t)(v >> 32));
	xdr_put_u32(x, (uint32_t)(v & 0xFFFFFFFFu));
}

void
xdr_put_bool(struct xdr *x, int v)
{
	xdr_put_u32(x, v ? 1 : 0);
}

void
xdr_put_fixed(struct xdr *x, const void *data, size_t n)
{
	size_t padded;
	unsigned char *p;

	if (n > x->len) {
	x->err = 1;
	return;
	}
	padded = XDR_PAD(n);
	p = xdr_adv(x, padded);
	if (p == NULL)
	return;
	memcpy(p, data, n);
	memset(p + n, 0, padded - n);
}

void
xdr_put_var(struct xdr *x, const void *data, size_t n)
{
	if (n > x->len) {
	x->err = 1;
	return;
	}
	xdr_put_u32(x, (uint32_t)n);
	xdr_put_fixed(x, data, n);
}

void
xdr_put_string(struct xdr *x, const char *s)
{
	xdr_put_var(x, s, strlen(s));
}

void
xdr_patch_u32(struct xdr *x, size_t at, uint32_t v)
{
	if (x->err || at > x->pos || x->pos - at < 4) {
	x->err = 1;
	return;
	}
	x->base[at]     = (unsigned char)(v >> 24);
	x->base[at + 1] = (unsigned char)(v >> 16);
	x->base[at + 2] = (unsigned char)(v >> 8);
	x->base[at + 3] = (unsigned char)v;
}
