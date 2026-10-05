/*
 * Simple NFSv2 READDIR client to test server reply format.
 * Sends MNT to get fh, then READDIR with that fh.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define SERVER "10.0.0.186"
#define MNT_PORT 1058
#define NFS_PORT 2049

static void hex_dump(const char *label, const uint8_t *data, size_t len)
{
	size_t i;
	printf("%s (%zu bytes):\n", label, len);
	for (i = 0; i < len; i++) {
	 printf("%02x ", data[i]);
	 if ((i + 1) % 16 == 0) printf("\n");
	}
	if (len % 16) printf("\n");
	printf("\n");
}

static uint32_t pack_u32(uint32_t v) { return htonl(v); }
static uint32_t unpack_u32(uint32_t v) { return ntohl(v); }

static int send_rpc(int fd, const char *ip, uint16_t port,
    uint32_t prog, uint32_t vers, uint32_t proc,
    const uint8_t *args, size_t args_len,
    uint8_t *reply, size_t reply_max)
{
	struct sockaddr_in sa;
	uint8_t req[1024];
	int pos = 0;
	uint32_t xid = 0x12345678;
	ssize_t n;
	socklen_t slen;
	struct sockaddr_in from;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
	 fprintf(stderr, "Bad IP\n");
	 return -1;
	}

	/* RPC header */
	uint32_t hdr[8];
	hdr[0] = pack_u32(xid);
	hdr[1] = pack_u32(0);  /* call */
	hdr[2] = pack_u32(2);  /* RPC v2 */
	hdr[3] = pack_u32(prog);
	hdr[4] = pack_u32(vers);
	hdr[5] = pack_u32(proc);
	hdr[6] = pack_u32(1);  /* AUTH_SYS */
	hdr[7] = pack_u32(0);  /* cred len */
	memcpy(req, hdr, 32);
	pos = 32;

	/* cred body (0 bytes, pad to 4) */
	req[pos++] = 0; req[pos++] = 0; req[pos++] = 0; req[pos++] = 0;
	/* verf body (0 bytes, pad to 4) */
	req[pos++] = 0; req[pos++] = 0; req[pos++] = 0; req[pos++] = 0;

	/* args */
	memcpy(req + pos, args, args_len);
	pos += args_len;

	/* pad to 4 */
	while (pos % 4) pos++;

	printf("Sending %d bytes to %s:%u prog=%u vers=%u proc=%u\n",
	    pos, ip, port, prog, vers, proc);

	if (sendto(fd, req, pos, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
	 perror("sendto");
	 return -1;
	}

	slen = sizeof(from);
	n = recvfrom(fd, reply, reply_max, 0, (struct sockaddr *)&from, &slen);
	if (n < 0) {
	 perror("recvfrom");
	 return -1;
	}

	return (int)n;
}

int main(void)
{
	int fd;
	uint8_t reply[4096];
	int n;
	uint32_t fh[8];  /* 32 bytes */
	uint8_t args[64];
	uint32_t *p;
	int i, pos;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	/* Step 1: MNT to get fh for /mnt/1 */
	printf("=== Step 1: MNT /mnt/1 ===\n");
	const char *path = "/mnt/1";
	int plen = strlen(path);
	p = (uint32_t *)args;
	p[0] = pack_u32(plen);  /* path len */
	memcpy(args + 4, path, plen);
	for (i = 4 + plen; i < 4 + ((plen + 3) & ~3); i++)
	 args[i] = 0;
	n = send_rpc(fd, SERVER, MNT_PORT, 100005, 1, 1, args, 4 + ((plen + 3) & ~3),
	    reply, sizeof(reply));
	if (n < 0) return 1;
	printf("MNT reply: %d bytes\n", n);
	hex_dump("MNT reply", reply, (size_t)n);

	/* Parse fh from reply */
	pos = 0;
	uint32_t rxid = unpack_u32(*(uint32_t *)(reply + pos));
	uint32_t rtype = unpack_u32(*(uint32_t *)(reply + pos + 4));
	uint32_t rstat = unpack_u32(*(uint32_t *)(reply + pos + 8));
	printf("xid=%08x type=%u stat=%u\n", rxid, rtype, rstat);
	pos += 12;  /* skip xid, type, stat */
	/* Skip verifier */
	uint32_t verf_flav = unpack_u32(*(uint32_t *)(reply + pos));
	uint32_t verf_len = unpack_u32(*(uint32_t *)(reply + pos + 4));
	printf("verf_flav=%u verf_len=%u\n", verf_flav, verf_len);
	pos += 8;
	pos += verf_len;
	/* Skip accept stat */
	uint32_t accept_stat = unpack_u32(*(uint32_t *)(reply + pos));
	printf("accept_stat=%u\n", accept_stat);
	pos += 4;

	/* Parse MNT result */
	uint32_t mnt_stat = unpack_u32(*(uint32_t *)(reply + pos));
	printf("MNT status=%u\n", mnt_stat);
	if (mnt_stat != 0) { fprintf(stderr, "MNT failed\n"); return 1; }
	pos += 4;
	memcpy(fh, reply + pos, 32);

	printf("fh: ");
	for (i = 0; i < 32; i++) printf("%02x", ((uint8_t *)fh)[i]);
	printf("\n\n");

	/* Step 2: READDIR with the fh */
	printf("=== Step 2: READDIR ===\n");
	p = (uint32_t *)args;
	p[0] = pack_u32(0);  /* offset */
	p[1] = pack_u32(1024);  /* count */
	n = send_rpc(fd, SERVER, NFS_PORT, 100003, 2, 16, args, 8,
	    reply, sizeof(reply));
	if (n < 0) return 1;
	printf("READDIR reply: %d bytes\n", n);
	hex_dump("READDIR reply", reply, (size_t)n);

	/* Parse reply */
	pos = 0;
	printf("RPC xid=%08x\n", unpack_u32(*(uint32_t *)(reply)));
	printf("RPC type=%u\n", unpack_u32(*(uint32_t *)(reply + 4)));
	printf("RPC stat=%u\n", unpack_u32(*(uint32_t *)(reply + 8)));
	pos += 12;  /* skip verifier */
	accept_stat = unpack_u32(*(uint32_t *)(reply + pos));
	printf("accept_stat=%u\n", accept_stat);
	pos += 4;

	/* Parse READDIR reply */
	uint32_t more = unpack_u32(*(uint32_t *)(reply + pos));
	printf("more=%u\n", more);
	pos += 4;

	int entry = 0;
	while (more && pos + 8 <= n) {
	 uint32_t cookie = unpack_u32(*(uint32_t *)(reply + pos));
	 uint32_t namelen = unpack_u32(*(uint32_t *)(reply + pos + 4));
	 printf("Entry %d: cookie=%u namelen=%u name=", entry++, cookie, namelen);
	 pos += 8;
	 if (pos + namelen > n) break;
	 for (i = 0; i < namelen; i++) putchar(reply[pos + i]);
	 printf("\n");
	 pos += (namelen + 3) & ~3;  /* padded */
	 if (pos + 8 > n) break;
	 uint32_t next_cookie = unpack_u32(*(uint32_t *)(reply + pos));
	 more = unpack_u32(*(uint32_t *)(reply + pos + 4));
	 printf("  next_cookie=%u more=%u\n", next_cookie, more);
	 pos += 8;
	}

	if (pos + 4 <= n) {
	 uint32_t eof = unpack_u32(*(uint32_t *)(reply + pos));
	 printf("eof=%u\n", eof);
	}

	close(fd);
	return 0;
}
