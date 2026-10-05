#ifndef CONF_H
#define CONF_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <stdint.h>

#define MAX_EXPORTS	64
#define MAX_PATH_LEN	1024
#define MAX_GIDS	16

struct export {
	char             path[MAX_PATH_LEN];
	struct in_addr   net;
	struct in_addr   mask;
	uint32_t         maproot_uid, maproot_gid;
	int              maproot_set;
	int              alldirs;
	int              ro;
	fsid_t           fsid;
};

int  conf_load(const char *path);
int  conf_reload(void);
const struct export *conf_lookup(const char *path, struct in_addr client);
const struct export *conf_get_export(unsigned idx);
unsigned             conf_export_count(void);

#endif
