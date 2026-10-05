#ifndef XDR_H
#define XDR_H

#include <stddef.h>
#include <stdint.h>

/* Round n up to a multiple of 4. */
#define XDR_PAD(n)	(((size_t)(n) + 3u) & ~(size_t)3u)

struct xdr {
	unsigned char *base;
	size_t         len;	/* buffer size */
	size_t         pos;	/* read or write offset */
	int            err;	/* sticky: set on any overrun or bad value */
};

void	 xdr_init(struct xdr *x, void *buf, size_t len);
int	 xdr_ok(const struct xdr *x);
size_t	 xdr_pos(const struct xdr *x);
size_t	 xdr_left(const struct xdr *x);

/* Decode. On error: return 0 and set x->err. */
uint32_t xdr_get_u32(struct xdr *x);
uint64_t xdr_get_u64(struct xdr *x);
int	 xdr_get_bool(struct xdr *x);
void	 xdr_get_fixed(struct xdr *x, void *out, size_t n);
/* Variable opaque. Rejects length > maxlen. maxlen must be < 2^31. */
size_t	 xdr_get_var(struct xdr *x, void *out, size_t maxlen);
/* String. "out" must hold maxlen + 1 bytes. Rejects embedded NUL. */
size_t	 xdr_get_string(struct xdr *x, char *out, size_t maxlen);
void	 xdr_skip_var(struct xdr *x, size_t maxlen);

/* Encode. On overflow: set x->err. */
void	 xdr_put_u32(struct xdr *x, uint32_t v);
void	 xdr_put_u64(struct xdr *x, uint64_t v);
void	 xdr_put_bool(struct xdr *x, int v);
void	 xdr_put_fixed(struct xdr *x, const void *p, size_t n);
void	 xdr_put_var(struct xdr *x, const void *p, size_t n);
void	 xdr_put_string(struct xdr *x, const char *s);
/* Overwrite a u32 at an earlier offset (record mark, counts). */
void	 xdr_patch_u32(struct xdr *x, size_t at, uint32_t v);

#endif
