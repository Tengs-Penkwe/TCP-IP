#ifndef __NETSTACK_NETWORK_H__
#define __NETSTACK_NETWORK_H__

#include <device/device.h>
#include "ethernet.h"
#include "ip.h"
#include "icmp.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"

typedef struct net_work {
    NetDevice  *device;
    Ethernet   *ether;
    ARP        *arp;
    IP         *ip;
    ICMP       *icmp;
    UDP        *udp;
    TCP        *tcp;
    ip_addr_t   my_ipv4;
    ipv6_addr_t my_ipv6;
    mac_addr   my_mac;
} NetWork;

__BEGIN_DECLS

errval_t network_init(NetWork* net, NetDevice* device);
void network_destroy(NetWork* net);

__END_DECLS

#endif // __NETSTACK_NETWORK_H__