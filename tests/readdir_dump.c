/*
 * Capture NFSv2 READDIR reply from server.
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

static int send_rpc_authsys(int fd, const char *ip, uint16_t port,
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
	const char *machine = "test";
	uint32_t stamp = 12345;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
	 fprintf(stderr, "Bad IP\n");
	 return -1;
	}

	/* Build AUTH_SYS credential */
	uint8_t cred[64];
	int cpos = 0;
	*(uint32_t *)(cred + cpos) = pack_u32(stamp); cpos += 4;
	int mlen = strlen(machine);
	*(uint32_t *)(cred + cpos) = pack_u32(mlen); cpos += 4;
	memcpy(cred + cpos, machine, mlen); cpos += mlen;
	while (cpos % 4) cred[cpos++] = 0;
	*(uint32_t *)(cred + cpos) = pack_u32(0); cpos += 4; /* uid */
	*(uint32_t *)(cred + cpos) = pack_u32(0); cpos += 4; /* gid */
	*(uint32_t *)(cred + cpos) = pack_u32(1); cpos += 4; /* gids count */
	*(uint32_t *)(cred + cpos) = pack_u32(0); cpos += 4; /* gid 0 */
	/* cpos is now the total cred size including padding */
	int cred_size = cpos;

	/* RPC header */
	uint32_t hdr[8];
	hdr[0] = pack_u32(xid);
	hdr[1] = pack_u32(0);  /* call */
	hdr[2] = pack_u32(2);  /* RPC v2 */
	hdr[3] = pack_u32(prog);
	hdr[4] = pack_u32(vers);
	hdr[5] = pack_u32(proc);
	hdr[6] = pack_u32(1);  /* AUTH_SYS */
	hdr[7] = pack_u32(cred_size);  /* cred len */
	memcpy(req, hdr, 32);
	pos = 32;

	/* cred body */
	memcpy(req + pos, cred, cred_size);
	pos += cred_size;

	/* verf body (0 bytes) */
	req[pos++] = 0; req[pos++] = 0; req[pos++] = 0; req[pos++] = 0;

	/* args */
	memcpy(req + pos, args, args_len);
	pos += args_len;

	/* pad to 4 */
	while (pos % 4) pos++;

	printf("Sending %d bytes\n", pos);
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

int main(int argc, char **argv)
{
	int fd;
	uint8_t reply[4096];
	int n;
	uint32_t fh[8];  /* 32 bytes */
	uint8_t args[64];
	uint32_t *p;
	int i, pos;
	uint16_t mnt_port = 1058;
	const char *path = "/mnt/1";

	if (argc > 1) mnt_port = atoi(argv[1]);
	if (argc > 2) path = argv[2];

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	/* Step 1: MNT to get fh */
	printf("=== MNT %s (port %u) ===\n", path, mnt_port);
	int plen = strlen(path);
	p = (uint32_t *)args;
	p[0] = pack_u32(plen);
	memcpy(args + 4, path, plen);
	for (i = 4 + plen; i < 4 + ((plen + 3) & ~3); i++)
	 args[i] = 0;
	n = send_rpc_authsys(fd, SERVER, mnt_port, 100005, 1, 1,
	    args, 4 + ((plen + 3) & ~3), reply, sizeof(reply));
	if (n < 0) return 1;
	printf("MNT reply: %d bytes\n", n);
	hex_dump("MNT reply", reply, (size_t)n);

	/* Parse fh from reply */
	pos = 24;  /* skip RPC header */
	uint32_t mnt_stat = unpack_u32(*(uint32_t *)(reply + pos));
	printf("MNT status=%u\n", mnt_stat);
	if (mnt_stat != 0) { fprintf(stderr, "MNT failed\n"); return 1; }
	pos += 4;
	memcpy(fh, reply + pos, 32);

	printf("fh: ");
	for (i = 0; i < 32; i++) printf("%02x", ((uint8_t *)fh)[i]);
	printf("\n\n");

	/* Step 2: READDIR with the fh */
	printf("=== READDIR ===\n");
	memcpy(args, fh, 32);
	p = (uint32_t *)(args + 32);
	p[0] = pack_u32(0);  /* offset */
	p[1] = pack_u32(1024);  /* count */
	n = send_rpc_authsys(fd, SERVER, NFS_PORT, 100003, 2, 16,
	    args, 40, reply, sizeof(reply));
	if (n < 0) return 1;
	printf("READDIR reply: %d bytes\n", n);
	hex_dump("READDIR reply", reply, (size_t)n);

	close(fd);
	return 0;
}
