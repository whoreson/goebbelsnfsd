#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#ifndef PROGNAME
#define PROGNAME "unfsd"
#endif

#define LOG_LINE	1024

static int  log_fd = 2;
static int  log_lvl = L_INFO;
static char log_file[1024];
static const char *const names[] = { "ERR", "WARN", "INFO", "DEBUG" };

int
log_open(const char *path)
{
	int fd;

	if (path == NULL) {
		log_file[0] = '\0';
		log_fd = 2;
		return 0;
	}
	if (strlen(path) >= sizeof(log_file)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0640);
	if (fd < 0)
		return -1;
	if (log_fd > 2)
		(void)close(log_fd);
	(void)strcpy(log_file, path);
	log_fd = fd;
	return 0;
}

int
log_reopen(void)
{
	if (log_file[0] == '\0')
		return 0;
	return log_open(log_file);
}

void
log_set_level(int level)
{
	log_lvl = level;
}

void
log_quiet_stderr(void)
{
	int fd;

	if (log_fd != 2)
		return;
	fd = open("/dev/null", O_WRONLY);
	if (fd >= 0) {
		(void)dup2(fd, 2);
		if (fd > 2)
			(void)close(fd);
	}
}

void
log_msg(int level, const char *fmt, ...)
{
	char line[LOG_LINE];
	char ts[32];
	time_t now;
	struct tm *tm;
	va_list ap;
	int n, k;

	if (level > log_lvl)
		return;
	if (level < L_ERR)
		level = L_ERR;
	if (level > L_DEBUG)
		level = L_DEBUG;

	now = time(NULL);
	tm = localtime(&now);
	ts[0] = '\0';
	if (tm != NULL)
		(void)strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

	#ifndef PROGNAME
#define PROGNAME "unfsd"
#endif

	n = snprintf(line, sizeof(line), "%s %s[%ld] %s: ", ts, PROGNAME,
	    (long)getpid(), names[level]);
	if (n < 0 || (size_t)n >= sizeof(line) - 2)
		return;
	va_start(ap, fmt);
	k = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (k < 0)
		return;
	n += k;
	if ((size_t)n > sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	(void)write(log_fd, line, (size_t)n);
}
