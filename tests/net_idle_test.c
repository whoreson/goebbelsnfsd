/* TCP idle timeout: a silent client is closed, an active one is kept. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "net.h"

static volatile sig_atomic_t quit_flag, hup_flag;

static void nohup_cb(void) {}

static int
connect_to(unsigned short port)
{
	struct sockaddr_in sin;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* 1 = peer closed (EOF), 0 = still open */
static int
closed(int fd)
{
	char b;
	struct pollfd p = { fd, POLLIN, 0 };

	if (poll(&p, 1, 0) <= 0)
		return 0;
	return read(fd, &b, 1) == 0;
}

int
main(void)
{
	unsigned short port = (unsigned short)(20000 + getpid() % 20000);
	pid_t child;
	int a, b, i, fails = 0;

	child = fork();
	if (child == 0) {
		if (net_tcp_open(port) < 0)
			_exit(2);
		net_set_idle_timeout(2);
		net_loop(&quit_flag, &hup_flag, nohup_cb);
		_exit(0);
	}
	usleep(300000);
	a = connect_to(port);
	b = connect_to(port);
	if (a < 0 || b < 0) {
		printf("FAIL: connect\n");
		kill(child, SIGKILL);
		return 1;
	}
	/* B sends one byte (part of a record header) every second. */
	for (i = 0; i < 5; i++) {
		sleep(1);
		if (write(b, "\0", 1) != 1)
			break;
	}
	if (!closed(a)) {
		printf("FAIL: idle connection was not closed\n");
		fails++;
	}
	if (closed(b)) {
		printf("FAIL: active connection was closed\n");
		fails++;
	}
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	if (fails == 0)
		printf("net_idle_test: OK\n");
	return fails != 0;
}
