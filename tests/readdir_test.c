/*
 * Test program: send NFSv2 READDIR to our server and dump the reply.
 * Uses raw UDP sockets to avoid kernel NFS client quirks.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static uint32_t htonl_u32(uint32_t v)
{
	return htonl(v);
}

static void dump_hex(const char *label, const uint8_t *data, size_t len)
{
	size_t i;
	printf("%s (%zu bytes):\n", label, len);
	for (i = 0; i < len; i++) {
	 printf("%02x ", data[i]);
	 if ((i + 1) % 16 == 0) printf("\n");
	}
	printf("\n\n");
}

static void build_readdir_req(uint8_t *buf, uint32_t xid,
    const uint8_t *fh, uint32_t cookie, uint32_t count)
{
	int pos = 0;
	/* RPC header */
	uint32_t f[8];
	f[0] = htonl_u32(xid);          /* xid */
	f[1] = htonl_u32(0);            /* call */
	f[2] = htonl_u32(2);            /* RPC v2 */
	f[3] = htonl_u32(100003);       /* NFS prog */
	f[4] = htonl_u32(2);            /* NFS v2 */
	f[5] = htonl_u32(16);           /* READDIR proc */
	f[6] = htonl_u32(1);            /* AUTH_SYS */
	f[7] = htonl_u32(0);            /* cred len (empty) */
	memcpy(buf, f, 32);
	pos = 32;
	/* cred body (0 bytes, padded to 4) */
	buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0;
	/* verf body (0 bytes, padded to 4) */
	buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0;
	/* fh_handle: 32 bytes */
	memcpy(buf + pos, fh, 32);
	pos += 32;
	/* cookie */
	f[0] = htonl_u32(cookie);
	memcpy(buf + pos, f, 4); pos += 4;
	/* count */
	f[0] = htonl_u32(count);
	memcpy(buf + pos, f, 4); pos += 4;
	/* Pad to 4 bytes */
	while (pos % 4) pos++;
	printf("Request: %d bytes\n", pos);
}

int main(int argc, char **argv)
{
	int fd;
	struct sockaddr_in sa;
	uint8_t req[256];
	uint8_t reply[4096];
	ssize_t n;
	uint32_t *p;
	int pos;

	if (argc < 2) {
	 fprintf(stderr, "Usage: %s <server_ip> [port]\n", argv[0]);
	 return 1;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(argc > 2 ? atoi(argv[2]) : 2049);
	if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1) {
	 fprintf(stderr, "Bad IP\n");
	 return 1;
	}

	/* Use a dummy 32-byte fh (all zeros for testing) */
	uint8_t fh[32];
	memset(fh, 0, sizeof(fh));

	/* Actually, we need the real fh from MNT. Skip that for now.
	 * Instead, let's use the fh that the MOUNT proc gave us.
	 * For this test, we'll send a READDIR with a known fh. */

	/* For now, just send a NULL call to verify connectivity */
	build_readdir_req(req, 0x12345678, fh, 0, 1024);

	if (sendto(fd, req, 52, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
	 perror("sendto");
	 return 1;
	}

	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	n = recvfrom(fd, reply, sizeof(reply), 0,
	    (struct sockaddr *)&from, &fromlen);
	if (n < 0) { perror("recvfrom"); return 1; }

	dump_hex("Reply", reply, (size_t)n);

	/* Parse reply */
	pos = 0;
	p = (uint32_t *)(reply + pos);
	printf("xid=%08x\n", ntohl(p[0]));
	printf("type=%u (1=reply)\n", ntohl(p[1]));
	printf("stat=%u (0=accepted)\n", ntohl(p[2]));
	pos += 32; /* RPC header + cred + verf */
	printf("accept_stat=%u (0=success)\n", ntohl(p[pos/4 - 1]));
	pos += 4; /* accept stat */
	/* Skip verifier */
	pos += 4; /* verf len */

	printf("Reply data starts at offset %d (%zd bytes remaining)\n",
	    pos, n - pos);
	dump_hex("Reply data", reply + pos, (size_t)(n - pos));

	close(fd);
	return 0;
}
