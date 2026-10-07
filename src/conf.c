#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "log.h"
#include "port.h"

static struct export *exports;
static unsigned nexports;
static unsigned exports_cap;
static char exports_file[MAX_PATH_LEN] = "/etc/exports";

static int
parse_addr(const char *s, struct in_addr *addr)
{
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	if (inet_pton(AF_INET, s, &sin.sin_addr) != 1)
	return -1;
	*addr = sin.sin_addr;
	return 0;
}

static int
exports_grow(void)
{
	struct export *new;
	unsigned cap = exports_cap == 0 ? 16 : exports_cap * 2;
	new = realloc(exports, cap * sizeof(*exports));
	if (new == NULL)
	return -1;
	exports = new;
	exports_cap = cap;
	return 0;
}

static int
parse_mask(const char *s, struct in_addr *addr)
{
	struct in_addr net;
	char buf[64];
	const char *slash;
	int bits;

	slash = strchr(s, '/');
	if (slash != NULL) {
	snprintf(buf, sizeof(buf), "%.*s", (int)(slash - s), s);
	if (parse_addr(buf, &net) < 0)
	return -1;
	bits = atoi(slash + 1);
	if (bits < 0 || bits > 32)
	return -1;
	addr->s_addr = htonl(((uint32_t)0xFFFFFFFFu) << (32 - bits));
	(void)net;
	} else {
	if (parse_addr(s, addr) < 0)
	return -1;
	}
	return 0;
}

static uint32_t
parse_uid(const char *s, int *ok)
{
	struct passwd *pw;
	char *end;
	unsigned long v;

	*ok = 0;
	v = strtoul(s, &end, 10);
	if (*end == '\0' && v <= 0xFFFFFFFFu) {
	*ok = 1;
	return (uint32_t)v;
	}
	pw = getpwnam(s);
	if (pw != NULL) {
	*ok = 1;
	return (uint32_t)pw->pw_uid;
	}
	return 0;
}

static int
parse_exports(const char *path)
{
	FILE *f;
	char line[2048];
	char lpath[MAX_PATH_LEN];
	struct export *ex;
	char *tok, *save;
	int rc;

	if (path == NULL)
	path = exports_file;
	f = fopen(path, "r");
	if (f == NULL) {
	log_msg(L_ERR, "cannot open %s: %s", path, strerror(errno));
	return -1;
	}

	/* Count lines first to check limits */
	nexports = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
	char *p;

	/* Strip comments and trailing whitespace */
	p = strchr(line, '#');
	if (p != NULL)
	*p = '\0';
	while (strlen(line) > 0 &&
	    (line[strlen(line) - 1] == '\n' ||
	     line[strlen(line) - 1] == '\t' ||
	     line[strlen(line) - 1] == ' '))
	line[strlen(line) - 1] = '\0';
	if (line[0] == '\0')
	continue;

	if (nexports >= exports_cap) {
	if (exports_grow() < 0) {
	log_msg(L_WARN, "cannot grow exports array");
	break;
	}
	}

	ex = &exports[nexports];
	memset(ex, 0, sizeof(*ex));
	ex->net.s_addr = htonl(0);	/* allow all */
	ex->mask.s_addr = htonl(0);

	/* Tokenize */
	save = line;
	tok = strtok_r(save, " \t", &save);
	if (tok == NULL)
	continue;
	if (strlen(tok) >= sizeof(ex->path)) {
	log_msg(L_WARN, "path too long: %s", tok);
	continue;
	}
	strcpy(ex->path, tok);

	/* Parse options */
	while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
	if (strcmp(tok, "-ro") == 0)
	ex->ro = 1;
	else if (strcmp(tok, "-alldirs") == 0)
	ex->alldirs = 1;
	else if (strncmp(tok, "-maproot=", 9) == 0) {
	uint32_t uid, gid;
	int ok;
	char *dot;

	uid = parse_uid(tok + 9, &ok);
	if (!ok) {
	log_msg(L_WARN, "bad -maproot value: %s", tok + 9);
	continue;
	}
	dot = strchr(tok + 9, '.');
	if (dot != NULL)
	gid = parse_uid(dot + 1, &ok);
	else
	gid = uid;
	ex->maproot_uid = uid;
	ex->maproot_gid = gid;
	ex->maproot_set = 1;
	} else if (strncmp(tok, "-mapall=", 8) == 0) {
	/* Same as -maproot for now */
	uint32_t uid, gid;
	int ok;
	char *dot;

	uid = parse_uid(tok + 8, &ok);
	if (!ok) {
	log_msg(L_WARN, "bad -mapall value: %s", tok + 8);
	continue;
	}
	dot = strchr(tok + 8, '.');
	if (dot != NULL)
	gid = parse_uid(dot + 1, &ok);
	else
	gid = uid;
	ex->maproot_uid = uid;
	ex->maproot_gid = gid;
	ex->maproot_set = 1;
	} else if (strcmp(tok, "-network") == 0) {
	tok = strtok_r(NULL, " \t", &save);
	if (tok == NULL) {
	log_msg(L_WARN, "missing -network address");
	continue;
	}
	if (parse_addr(tok, &ex->net) < 0) {
	log_msg(L_WARN, "bad -network address: %s", tok);
	continue;
	}
	} else if (strcmp(tok, "-mask") == 0) {
	tok = strtok_r(NULL, " \t", &save);
	if (tok == NULL) {
	log_msg(L_WARN, "missing -mask address");
	continue;
	}
	if (parse_mask(tok, &ex->mask) < 0) {
	log_msg(L_WARN, "bad -mask: %s", tok);
	continue;
	}
	} else {
	/* Bare address (host or network) */
	if (parse_addr(tok, &ex->net) == 0) {
	/* Default mask: treat as single host (/32) */
	ex->mask.s_addr = 0xFFFFFFFFu;
	} else {
	log_msg(L_WARN, "unknown option: %s", tok);
	}
	}
	}

	/* Resolve the path and get fsid */
	lpath[0] = '\0';
	if (realpath(ex->path, lpath) == NULL) {
	log_msg(L_WARN, "cannot resolve %s: %s", ex->path,
	    strerror(errno));
	continue;
	}
	strcpy(ex->path, lpath);

	if (port_lgetfh(ex->path, (fhandle_t *)0) < 0) {
	/* We need the fsid, not the full handle. Use statfs. */
	struct statfs sf;
	if (statfs(ex->path, &sf) < 0) {
	log_msg(L_WARN, "statfs %s: %s", ex->path,
	    strerror(errno));
	continue;
	}
	ex->fsid_val[0] = PORT_FSID_VAL0(sf);
	ex->fsid_val[1] = PORT_FSID_VAL1(sf);
	} else {
	/* We already have it from statfs above */
	struct statfs sf;
	if (statfs(ex->path, &sf) < 0) {
	log_msg(L_WARN, "statfs %s: %s", ex->path,
	    strerror(errno));
	continue;
	}
	ex->fsid_val[0] = PORT_FSID_VAL0(sf);
	ex->fsid_val[1] = PORT_FSID_VAL1(sf);
	}

	log_msg(L_INFO, "export: %s (fsid=%lx:%lx) ro=%d alldirs=%d "
	    "net=%s",
	    ex->path,
	    (unsigned long)ex->fsid_val[0],
	    (unsigned long)ex->fsid_val[1],
	    ex->ro, ex->alldirs,
	    inet_ntoa(ex->net));
	nexports++;
	}

	rc = fclose(f);
	if (rc < 0)
	log_msg(L_WARN, "fclose: %s", strerror(errno));
	return rc < 0 ? -1 : 0;
}

int

conf_load(const char *path)
{
	if (path != NULL)
	(void)strcpy(exports_file, path);
	nexports = 0;
	exports_cap = 0;
	free(exports);
	exports = NULL;
	return parse_exports(path);
}

int
conf_reload(void)
{
	return conf_load(NULL);
}

const struct export *
conf_lookup(const char *path, struct in_addr client)
{
	unsigned i;
	const struct export *ex;

	for (i = 0; i < nexports; i++) {
	ex = &exports[i];

	/* Check network access */
	if (ex->net.s_addr != htonl(0)) {
	if ((client.s_addr & ex->mask.s_addr) != ex->net.s_addr)
	continue;
	}

	/* Check path */
	if (ex->alldirs) {
	/* Path must start with export path */
	size_t elen = strlen(ex->path);
	if (strncmp(path, ex->path, elen) == 0 &&
	    (path[elen] == '\0' || path[elen] == '/')) {
	/* Verify same filesystem via realpath */
	char rp[MAX_PATH_LEN];
	struct statfs sf1, sf2;
	if (realpath(path, rp) == NULL)
	continue;
	if (statfs(rp, &sf1) < 0 || statfs(ex->path, &sf2) < 0)
	continue;
	if (PORT_FSID_VAL0(sf1) == PORT_FSID_VAL0(sf2) &&
	    PORT_FSID_VAL1(sf1) == PORT_FSID_VAL1(sf2))
	return ex;
	}
	} else {
	if (strcmp(path, ex->path) == 0)
	return ex;
	}
	}
	return NULL;
}

const struct export *
conf_get_export(unsigned idx)
{
	if (idx >= nexports)
	return NULL;
	return &exports[idx];
}

unsigned
conf_export_count(void)
{
	return nexports;
}
