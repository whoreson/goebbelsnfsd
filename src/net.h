#ifndef NET_H
#define NET_H

#include <signal.h>

/* Open a UDP socket on all addresses. Return 0 or -1. */
int  net_udp_open(unsigned short port);

/* Run until *quit is set. Call on_hup() when *hup is set. */
void net_loop(volatile sig_atomic_t *quit, volatile sig_atomic_t *hup,
    void (*on_hup)(void));

#endif
