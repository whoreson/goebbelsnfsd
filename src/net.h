#ifndef NET_H
#define NET_H

#include <signal.h>

/* Open a UDP socket on all addresses. Return 0 or -1. */
int  net_udp_open(unsigned short port);

/* Open a TCP listening socket on all addresses. Return 0 or -1. */
int  net_tcp_open(unsigned short port);

/*
 * Close a TCP connection that sends and receives nothing for "secs"
 * seconds (default 300). 0 turns the timeout off.
 */
void net_set_idle_timeout(int secs);

/* Run until *quit is set. Call on_hup() when *hup is set. */
void net_loop(volatile sig_atomic_t *quit, volatile sig_atomic_t *hup,
    void (*on_hup)(void));

#endif
