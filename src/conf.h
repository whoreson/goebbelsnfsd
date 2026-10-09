#ifndef CONF_H
#define CONF_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <stdint.h>

#include <limits.h>

#define MAX_PATH_LEN	1024
/* realpath() may write PATH_MAX bytes. Use this size for its buffer. */
#define REALPATH_BUF_LEN	PATH_MAX
#define MAX_GIDS	16

struct export {
	char             path[MAX_PATH_LEN];
	struct in_addr   net;
	struct in_addr   mask;
	uint32_t         maproot_uid, maproot_gid;
	int              maproot_set;
	int              mapall;	/* -mapall: every client user is mapped */
	int              alldirs;
	int              ro;
	uint32_t         fsid_val[2];
};

int  conf_load(const char *path);
int  conf_reload(void);
/* Return 1 if "client" may use export "ex" (-network / -mask). */
int  conf_client_ok(const struct export *ex, struct in_addr client);
/* Set the client of the request now being served (checked on every NFS call). */
void conf_set_client(struct in_addr client);
struct in_addr conf_get_client(void);
/*
 * Map the user of a request for this export (like FreeBSD exports(5)):
 *   -mapall=U:  every user becomes U.
 *   -maproot=U: uid 0 becomes U.
 *   neither:    uid 0 becomes "nobody" (root squash). Other users stay.
 */
#define CONF_NOBODY_UID	65534u
#define CONF_NOBODY_GID	65534u
void conf_map_cred(const struct export *ex, uint32_t *uid, uint32_t *gid);
const struct export *conf_lookup(const char *path, struct in_addr client);
const struct export *conf_get_export(unsigned idx);
unsigned             conf_export_count(void);

#endif
