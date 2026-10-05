#ifndef LOG_H
#define LOG_H

enum { L_ERR = 0, L_WARN = 1, L_INFO = 2, L_DEBUG = 3 };

/* path == NULL: log to stderr. Return 0 on success, -1 on error. */
int  log_open(const char *path);
int  log_reopen(void);	/* call on SIGHUP */
void log_set_level(int level);
void log_quiet_stderr(void);	/* after daemonize without -l */

#ifdef __GNUC__
void log_msg(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void log_msg(int level, const char *fmt, ...);
#endif

#endif
