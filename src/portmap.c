#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>

#include "log.h"
#include "portmap.h"

#define MAXENT	16

struct ent {
	unsigned long  prog, vers;
	int            prot;
	unsigned short port;
};

static struct ent tab[MAXENT];
static unsigned nent;

int
portmap_add(unsigned long prog, unsigned long vers, int prot,
    unsigned short port)
{
	if (nent >= MAXENT)
	return -1;
	tab[nent].prog = prog;
	tab[nent].vers = vers;
	tab[nent].prot = prot;
	tab[nent].port = port;
	nent++;
	return 0;
}

void
portmap_remove_all(void)
{
	unsigned i;

	for (i = 0; i < nent; i++)
	(void)pmap_unset(tab[i].prog, tab[i].vers);
}

int
portmap_commit(void)
{
	unsigned i;

	portmap_remove_all();	/* clear entries of a crashed instance */
	for (i = 0; i < nent; i++) {
	if (!pmap_set(tab[i].prog, tab[i].vers, tab[i].prot,
	    tab[i].port)) {
	log_msg(L_ERR, "pmap_set failed: prog %lu v%lu "
	    "port %u. Is rpcbind running?",
	    tab[i].prog, tab[i].vers,
	    (unsigned)tab[i].port);
	return -1;
	}
	log_msg(L_DEBUG, "registered prog %lu v%lu %s port %u",
	    tab[i].prog, tab[i].vers,
	    tab[i].prot == IPPROTO_UDP ? "udp" : "tcp",
	    (unsigned)tab[i].port);
	}
	return 0;
}
