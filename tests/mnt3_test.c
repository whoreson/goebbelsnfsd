#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

static void dump_hex(const uint8_t *p, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) printf("\n  ");
        printf("%02x ", p[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 1058;
    const char *path = argc > 3 ? argv[3] : "/mnt/1";

    struct sockaddr_in sin;
    int sd;
    uint8_t req[256], rep[1024];
    uint32_t *p;
    ssize_t n;
    socklen_t slen;

    sd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, host, &sin.sin_addr);

    /* Build MOUNT v3 request: MNT("/mnt/1") */
    p = (uint32_t *)req;
    *p++ = htonl(0x12345678);  /* xid */
    *p++ = htonl(0);           /* call */
    *p++ = htonl(2);           /* RPC v2 */
    *p++ = htonl(100005);      /* MOUNT prog */
    *p++ = htonl(3);           /* MOUNT v3 */
    *p++ = htonl(1);           /* MNT proc */
    *p++ = htonl(1);           /* AUTH_UNIX */
    *p++ = htonl(0);           /* stamp */
    /* auth_unix: machname(0) */
    *p++ = htonl(0);
    /* auth_unix: uid */
    *p++ = htonl(0);
    /* auth_unix: gid */
    *p++ = htonl(0);
    /* auth_unix: gids(0) */
    *p++ = htonl(0);
    /* MOUNT v3 MNT args: string path */
    *p++ = htonl(strlen(path));
    memcpy(p, path, strlen(path));
    p += (strlen(path) + 3) / 4;

    size_t req_len = (char*)p - (char*)req;

    printf("Request (%zu bytes):\n", req_len);
    dump_hex(req, req_len);

    if (sendto(sd, req, req_len, 0, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("sendto");
        return 1;
    }

    slen = sizeof(sin);
    n = recvfrom(sd, rep, sizeof(rep), 0, (struct sockaddr *)&sin, &slen);
    if (n < 0) {
        perror("recvfrom");
        return 1;
    }

    printf("Reply (%zd bytes):\n", n);
    dump_hex(rep, (size_t)n);

    /* Parse RPC reply */
    /* xid(4) + reply(4) + accept/cancel(20) */
    uint32_t msg_type = ntohl(((uint32_t*)rep)[1]);
    uint32_t stat = ntohl(((uint32_t*)rep)[2]);
    printf("msg_type=%u stat=%u\n", msg_type, stat);

    if (msg_type == 0 && stat == 1) {
        /* Accept reply: offset 8 from accept header start */
        uint32_t astat = ntohl(((uint32_t*)rep)[4]);
        printf("astat=%u\n", astat);
        if (astat == 0) {
            /* Procedure successful - parse MNT3 result */
            /* Status starts at offset 48 (xid+msgtype+stat+auth(16)+astat(4)+reserved(12) = 32+16 = 48? */
            /* Actually: xid(4)+msgtype(4)+stat(4)+cstat(4)+xida(4)+astat(4)+auth(4+n) */
            /* For AUTH_NONE: auth_len=4, so body starts at 28 */
            /* For AUTH_UNIX with empty machname/gids: auth_len=20 */
            /* Body starts at 8+20 = 28 */
            /* MNT3 status at offset 28 */
            uint32_t mnt_status = ntohl(((uint32_t*)rep)[28/4]);
            printf("MNT3 status at offset 28: %u\n", mnt_status);
            /* Actually let me just dump byte-by-byte from offset 24 */
            printf("From offset 24:\n");
            dump_hex(rep + 24, (size_t)(n - 24));
        }
    }

    close(sd);
    return 0;
}
