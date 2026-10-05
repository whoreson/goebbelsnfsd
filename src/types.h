#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <stdint.h>

#define NFS_PORT	2049
#define MOUNT_PORT_DEFAULT	1058

/* Transfer sizes. FSINFO reports these values. */
#define NFS2_MAXDATA	8192u
#define NFS3_MAXDATA_UDP	8192u
#define NFS3_MAXDATA_TCP	32768u

/* Largest RPC message we accept or send (data + headers). */
#define RPC_MAXMSG	(NFS3_MAXDATA_TCP + 1024u)

#endif
