#ifndef NET_NETCFG_H
#define NET_NETCFG_H

#include <stdbool.h>
#include <stdint.h>

/* This machine's network configuration: what DHCP leased it, or nothing yet.
 *
 * There used to be a compile-time 10.0.2.15 here, with a /24 mask and a gateway
 * at 10.0.2.2, because nothing could learn them. Now the network server starts
 * with none of it -- 0.0.0.0, unbound -- and dhcp.c fills this in when a server
 * acknowledges a lease, and clears it when the lease runs out. Until it is
 * bound, the machine answers nothing: an address it has not been given is not
 * one it may answer for.
 *
 * Every address here is in HOST order, like the constants it replaced; they
 * go onto the wire through htonl(). Kept in the network server; no other
 * process has a network configuration, because no other process has a card. */

typedef struct {
    bool     bound;         /* false until an ACK, and again after expiry     */
    uint32_t address;       /* this machine's address; 0 while unbound        */
    uint32_t netmask;       /* contiguous, /8 to /30                          */
    uint32_t router;        /* 0 if the lease named none, or none usable      */
    uint32_t server;        /* the DHCP server that granted the lease         */
    uint32_t lease_seconds; /* 0xFFFFFFFF means infinite                      */
} net_config_t;

/* The current configuration. Never NULL. */
const net_config_t *net_config(void);

/* Installs a lease. `lease->bound` is ignored; the result is bound. */
void net_config_bind(const net_config_t *lease);

/* Forgets the lease: back to 0.0.0.0 and unbound. */
void net_config_clear(void);

/* The subnet's own address and its broadcast address, for a bound
 * configuration. */
uint32_t net_config_network(void);
uint32_t net_config_broadcast(void);

#endif /* NET_NETCFG_H */
