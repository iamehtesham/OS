/* The network configuration the rest of the server reads. See netcfg.h. */

#include <stdbool.h>
#include <stdint.h>

#include "net/netcfg.h"

/* In .bss, so it starts all zeroes: unbound, 0.0.0.0. */
static net_config_t config;

const net_config_t *net_config(void)
{
    return &config;
}

/* Field by field rather than `config = *lease`: a struct copy is one of the
 * things GCC may turn into a memcpy call, and ring 3 has no memcpy to link. */
void net_config_bind(const net_config_t *lease)
{
    config.address       = lease->address;
    config.netmask       = lease->netmask;
    config.router        = lease->router;
    config.server        = lease->server;
    config.lease_seconds = lease->lease_seconds;
    config.bound         = true;
}

void net_config_clear(void)
{
    config.bound         = false;
    config.address       = 0;
    config.netmask       = 0;
    config.router        = 0;
    config.server        = 0;
    config.lease_seconds = 0;
}

uint32_t net_config_network(void)
{
    return config.address & config.netmask;
}

uint32_t net_config_broadcast(void)
{
    return net_config_network() | ~config.netmask;
}
