#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#define MAXCONN	128

struct conn {
	int                fd;
	struct sockaddr_in peer;
	struct in_addr     local;
	unsigned char      hdr[4];
	size_t             hdrlen;
	size_t             fragleft;
	int                last;
	size_t             inlen;
	size_t             outlen, outpos;
	unsigned char      in[RPC_MAXMSG];
	unsigned char      out[RPC_MAXMSG + 4];
};

static int socks[MAXSOCKS];
static unsigned nsocks;

static int lsocks[MAXSOCKS];
static unsigned nlisten;
static struct conn conns[MAXCONN];
static int conns_ready;

static unsigned char rbuf[RPC_MAXMSG];
static unsigned char sbuf[RPC_MAXMSG];

static void
conns_init(void)
{
	unsigned i;

	if (conns_ready)
	return;
	for (i = 0; i < MAXCONN; i++)
	conns[i].fd = -1;
	conns_ready = 1;
}

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

int
net_tcp_open(unsigned short port)
{
	struct sockaddr_in sin;
	int fd, on, fl;

	if (nlisten >= MAXSOCKS)
	return -1;
	conns_init();
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
	log_msg(L_ERR, "socket: %s", strerror(errno));
	return -1;
	}
	on = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0 ||
	    listen(fd, 128) < 0) {
	log_msg(L_ERR, "bind/listen tcp port %u: %s", (unsigned)port,
	    strerror(errno));
	(void)close(fd);
	return -1;
	}
	fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
	(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	lsocks[nlisten++] = fd;
	log_msg(L_INFO, "listening on tcp port %u", (unsigned)port);
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

static void
tcp_close(struct conn *c)
{
	log_msg(L_DEBUG, "tcp close %s:%u", inet_ntoa(c->peer.sin_addr),
	    ntohs(c->peer.sin_port));
	(void)close(c->fd);
	c->fd = -1;
}

static void
tcp_accept(int lfd)
{
	struct sockaddr_in from, loc;
	socklen_t len;
	struct conn *c;
	unsigned i;
	int fd, on, fl;

	len = sizeof(from);
	fd = accept(lfd, (struct sockaddr *)&from, &len);
	if (fd < 0) {
	if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED)
	log_msg(L_WARN, "accept: %s", strerror(errno));
	return;
	}
	c = NULL;
	for (i = 0; i < MAXCONN; i++)
	if (conns[i].fd < 0) {
	c = &conns[i];
	break;
	}
	if (c == NULL) {
	log_msg(L_WARN, "too many tcp connections, refused %s",
	    inet_ntoa(from.sin_addr));
	(void)close(fd);
	return;
	}
	fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
	(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	on = 1;
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	c->peer = from;
	c->local.s_addr = htonl(INADDR_ANY);
	len = sizeof(loc);
	if (getsockname(fd, (struct sockaddr *)&loc, &len) == 0)
	c->local = loc.sin_addr;
	log_msg(L_DEBUG, "tcp accept %s:%u", inet_ntoa(from.sin_addr),
	    ntohs(from.sin_port));
}

/* Write pending output. Return 0 (done or would block) or -1. */
static int
tcp_flush(struct conn *c)
{
	ssize_t n;

	while (c->outpos < c->outlen) {
	n = write(c->fd, c->out + c->outpos, c->outlen - c->outpos);
	if (n < 0) {
	if (errno == EINTR)
	continue;
	if (errno == EAGAIN)
	return 0;
	return -1;
	}
	c->outpos += (size_t)n;
	}
	c->outpos = c->outlen = 0;
	return 0;
}

/* Handle one complete message in c->in. Return 0 or -1. */
static int
tcp_dispatch(struct conn *c)
{
	struct req r;
	size_t rn;

	memset(&r, 0, sizeof(r));
	r.fd = c->fd;
	r.is_tcp = 1;
	r.peer = c->peer;
	r.local = c->local;
	rn = rpc_handle(&r, c->in, c->inlen, c->out + 4,
	    sizeof(c->out) - 4);
	if (rn == 0)
	return 0;
	c->out[0] = (unsigned char)(0x80 | ((rn >> 24) & 0x7F));
	c->out[1] = (unsigned char)(rn >> 16);
	c->out[2] = (unsigned char)(rn >> 8);
	c->out[3] = (unsigned char)rn;
	c->outlen = rn + 4;
	c->outpos = 0;
	return tcp_flush(c);
}

/* Read and process input. Return 0, or -1 to close the connection. */
static int
tcp_readable(struct conn *c)
{
	ssize_t n;
	int budget = 8, rc;

	while (c->outlen == 0 && budget > 0) {
	if (c->hdrlen == 4 && c->fragleft == 0) {
	c->hdrlen = 0;
	if (c->last) {
	budget--;
	rc = tcp_dispatch(c);
	c->inlen = 0;
	if (rc < 0)
	return -1;
	}
	continue;
	}
	if (c->hdrlen < 4)
	n = read(c->fd, c->hdr + c->hdrlen, 4 - c->hdrlen);
	else
	n = read(c->fd, c->in + c->inlen, c->fragleft);
	if (n == 0)
	return -1;
	if (n < 0) {
	if (errno == EINTR)
	continue;
	if (errno == EAGAIN)
	return 0;
	return -1;
	}
	if (c->hdrlen < 4) {
	c->hdrlen += (size_t)n;
	if (c->hdrlen == 4) {
	c->last = (c->hdr[0] & 0x80) != 0;
	c->fragleft = ((size_t)(c->hdr[0] & 0x7F) << 24) |
	    ((size_t)c->hdr[1] << 16) |
	    ((size_t)c->hdr[2] << 8) | (size_t)c->hdr[3];
	if (c->inlen + c->fragleft > RPC_MAXMSG) {
	log_msg(L_WARN, "oversize tcp record from %s",
	    inet_ntoa(c->peer.sin_addr));
	return -1;
	}
	}
	} else {
	c->inlen += (size_t)n;
	c->fragleft -= (size_t)n;
	}
	}
	return 0;
}

static void
tcp_event(int fd, short revents)
{
	struct conn *c = NULL;
	unsigned i;

	for (i = 0; i < MAXCONN; i++)
	if (conns[i].fd == fd) {
	c = &conns[i];
	break;
	}
	if (c == NULL)
	return;
	if (revents & (POLLERR | POLLNVAL)) {
	tcp_close(c);
	return;
	}
	if (c->outlen > c->outpos) {
	if ((revents & (POLLOUT | POLLHUP)) && tcp_flush(c) < 0)
	tcp_close(c);
	return;
	}
	if ((revents & (POLLIN | POLLHUP)) && tcp_readable(c) < 0)
	tcp_close(c);
}

void
net_loop(volatile sig_atomic_t *quit, volatile sig_atomic_t *hup,
    void (*on_hup)(void))
{
	struct pollfd pfd[MAXSOCKS * 2 + MAXCONN];
	unsigned i, np;
	int n;

	conns_init();
	while (!*quit) {
	if (*hup) {
	*hup = 0;
	on_hup();
	}
	np = 0;
	for (i = 0; i < nsocks; i++, np++) {
	pfd[np].fd = socks[i];
	pfd[np].events = POLLIN;
	pfd[np].revents = 0;
	}
	for (i = 0; i < nlisten; i++, np++) {
	pfd[np].fd = lsocks[i];
	pfd[np].events = POLLIN;
	pfd[np].revents = 0;
	}
	for (i = 0; i < MAXCONN; i++) {
	if (conns[i].fd < 0)
	continue;
	pfd[np].fd = conns[i].fd;
	pfd[np].events = conns[i].outlen > conns[i].outpos ?
	    POLLOUT : POLLIN;
	pfd[np].revents = 0;
	np++;
	}
	n = poll(pfd, np, 1000);
	if (n < 0) {
	if (errno == EINTR)
	continue;
	log_msg(L_ERR, "poll: %s", strerror(errno));
	return;
	}
	for (i = 0; i < np; i++) {
	if (pfd[i].revents == 0)
	continue;
	if (i < nsocks)
	handle_udp(pfd[i].fd);
	else if (i < nsocks + nlisten)
	tcp_accept(pfd[i].fd);
	else
	tcp_event(pfd[i].fd, pfd[i].revents);
	}
	}
}
