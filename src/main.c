#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "portmap.h"
#include "progs.h"
#include "rpc.h"
#include "types.h"

static volatile sig_atomic_t want_quit, want_hup;

static void
on_signal(int sig)
{
	if (sig == SIGHUP)
	want_hup = 1;
	else
	want_quit = 1;
}

static void
handle_hup(void)
{
	log_msg(L_INFO, "SIGHUP received");
	if (log_reopen() < 0)
	log_msg(L_ERR, "log reopen failed: %s", strerror(errno));
	/* M2: reload the exports file here. */
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-dv] [-l logfile] [-m mountport]\n"
	    "  -d  run in background\n"
	    "  -v  verbose: log each RPC call\n"
	    "  -l  log to file (reopened on SIGHUP)\n"
	    "  -m  MOUNT port (default %d)\n",
	    PROGNAME, MOUNT_PORT_DEFAULT);
	exit(2);
}

static int
daemonize(void)
{
	pid_t pid;
	int fd;

	pid = fork();
	if (pid < 0)
	return -1;
	if (pid > 0)
	_exit(0);
	(void)setsid();
	(void)chdir("/");
	fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
	(void)dup2(fd, 0);
	(void)dup2(fd, 1);
	(void)dup2(fd, 2);
	if (fd > 2)
	(void)close(fd);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	const char *logfile = NULL;
	unsigned long mport = MOUNT_PORT_DEFAULT;
	char *end;
	int ch, background = 0, verbose = 0;

	while ((ch = getopt(argc, argv, "dhl:m:v")) != -1) {
	switch (ch) {
	case 'd': background = 1; break;
	case 'l': logfile = optarg; break;
	case 'm':
	mport = strtoul(optarg, &end, 10);
	if (*optarg == '\0' || *end != '\0' ||
	    mport == 0 || mport > 65535UL)
	usage();
	break;
	case 'v': verbose++; break;
	default: usage();
	}
	}
	if (optind != argc)
	usage();

	if (logfile != NULL && log_open(logfile) < 0) {
	fprintf(stderr, "%s: cannot open %s: %s\n", PROGNAME,
	    logfile, strerror(errno));
	return 1;
	}
	log_set_level(L_INFO + verbose > L_DEBUG ? L_DEBUG : L_INFO + verbose);

	if (geteuid() != 0) {
	log_msg(L_ERR, "must run as root");
	return 1;
	}

	if (rpc_register(&nfs2_prog) < 0 || rpc_register(&nfs3_prog) < 0 ||
	    rpc_register(&mount1_prog) < 0 || rpc_register(&mount3_prog) < 0) {
	log_msg(L_ERR, "rpc_register failed");
	return 1;
	}

	if (net_udp_open(NFS_PORT) < 0 ||
	    net_udp_open((unsigned short)mport) < 0)
	return 1;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;	/* no SA_RESTART: poll() must wake */
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (background && daemonize() < 0) {
	log_msg(L_ERR, "fork: %s", strerror(errno));
	return 1;
	}

	portmap_add(RPC_PROG_NFS, 2, IPPROTO_UDP, NFS_PORT);
	portmap_add(RPC_PROG_NFS, 3, IPPROTO_UDP, NFS_PORT);
	portmap_add(RPC_PROG_MOUNT, 1, IPPROTO_UDP, (unsigned short)mport);
	portmap_add(RPC_PROG_MOUNT, 3, IPPROTO_UDP, (unsigned short)mport);
	if (portmap_commit() < 0)
	return 1;

	log_msg(L_INFO, "started");
	net_loop(&want_quit, &want_hup, handle_hup);

	portmap_remove_all();
	log_msg(L_INFO, "stopped");
	return 0;
}
