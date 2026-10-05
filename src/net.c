#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "rpc.h"
#include "types.h"

#define MAXSOCKS	8

static int socks[MAXSOCKS];
static unsigned nsocks;

static unsigned char rbuf[RPC_MAXMSG];
static unsigned char sbuf[RPC_MAXMSG];

int
net_udp_open(unsigned short port)
{
	struct sockaddr_in sin;
	int fd, on, bufsz, fl;

	if (nsocks >= MAXSOCKS)
	return -1;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
	log_msg(L_ERR, "socket: %s", strerror(errno));
	return -1;
	}
#ifdef IP_RECVDSTADDR
	on = 1;
	if (setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &on, sizeof(on)) < 0)
	log_msg(L_WARN, "IP_RECVDSTADDR: %s", strerror(errno));
#endif
	bufsz = 262144;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
	log_msg(L_ERR, "bind udp port %u: %s", (unsigned)port,
	    strerror(errno));
	(void)close(fd);
	return -1;
	}
	fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
	(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	socks[nsocks++] = fd;
	log_msg(L_INFO, "listening on udp port %u", (unsigned)port);
	return 0;
}

static void
handle_udp(int fd)
{
	struct sockaddr_in from;
	struct msghdr mh;
	struct iovec iov;
	struct cmsghdr *c;
	struct req r;
	struct in_addr local;
	union { struct cmsghdr h; char b[64]; } cm;
	ssize_t n;
	size_t rn;

	iov.iov_base = rbuf;
	iov.iov_len = sizeof(rbuf);
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = sizeof(from);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cm.b;
	mh.msg_controllen = sizeof(cm.b);

	n = recvmsg(fd, &mh, 0);
	if (n < 0) {
	if (errno != EAGAIN && errno != EINTR)
	log_msg(L_WARN, "recvmsg: %s", strerror(errno));
	return;
	}
	if (mh.msg_flags & MSG_TRUNC) {
	log_msg(L_DEBUG, "oversize datagram dropped");
	return;
	}

	local.s_addr = htonl(INADDR_ANY);
#ifdef IP_RECVDSTADDR
	for (c = CMSG_FIRSTHDR(&mh); c != NULL; c = CMSG_NXTHDR(&mh, c)) {
	if (c->cmsg_level == IPPROTO_IP &&
	    c->cmsg_type == IP_RECVDSTADDR &&
	    c->cmsg_len >= CMSG_LEN(sizeof(local)))
	memcpy(&local, CMSG_DATA(c), sizeof(local));
	}
#else
	(void)c;
#endif

	memset(&r, 0, sizeof(r));
	r.fd = fd;
	r.is_tcp = 0;
	r.peer = from;
	r.local = local;

	rn = rpc_handle(&r, rbuf, (size_t)n, sbuf, sizeof(sbuf));
	if (rn == 0)
	return;

	iov.iov_base = sbuf;
	iov.iov_len = rn;
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = sizeof(from);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
#ifdef IP_SENDSRCADDR
	if (local.s_addr != htonl(INADDR_ANY)) {
	memset(&cm, 0, sizeof(cm));
	mh.msg_control = cm.b;
	mh.msg_controllen = CMSG_SPACE(sizeof(local));
	c = CMSG_FIRSTHDR(&mh);
	c->cmsg_len = CMSG_LEN(sizeof(local));
	c->cmsg_level = IPPROTO_IP;
	c->cmsg_type = IP_SENDSRCADDR;
	memcpy(CMSG_DATA(c), &local, sizeof(local));
	}
#endif
	if (sendmsg(fd, &mh, 0) < 0)
	log_msg(L_WARN, "sendmsg to %s: %s",
	    inet_ntoa(from.sin_addr), strerror(errno));
}

void
net_loop(volatile sig_atomic_t *quit, volatile sig_atomic_t *hup,
    void (*on_hup)(void))
{
	struct pollfd pfd[MAXSOCKS];
	unsigned i;
	int n;

	for (i = 0; i < nsocks; i++) {
	pfd[i].fd = socks[i];
	pfd[i].events = POLLIN;
	}
	while (!*quit) {
	if (*hup) {
	*hup = 0;
	on_hup();
	}
	for (i = 0; i < nsocks; i++)
	pfd[i].revents = 0;
	n = poll(pfd, nsocks, 1000);
	if (n < 0) {
	if (errno == EINTR)
	continue;
	log_msg(L_ERR, "poll: %s", strerror(errno));
	return;
	}
	for (i = 0; i < nsocks; i++)
	if (pfd[i].revents & POLLIN)
	handle_udp(pfd[i].fd);
	}
}
