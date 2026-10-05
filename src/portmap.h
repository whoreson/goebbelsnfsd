#ifndef PORTMAP_H
#define PORTMAP_H

/* prot: IPPROTO_UDP or IPPROTO_TCP. Only records the entry. */
int  portmap_add(unsigned long prog, unsigned long vers, int prot,
    unsigned short port);
/* Remove stale entries, then register all recorded entries. */
int  portmap_commit(void);
void portmap_remove_all(void);

#endif
