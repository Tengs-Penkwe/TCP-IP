#ifndef __DEV_ETHERNET_H__
#define __DEV_ETHERNET_H__

#include <stdint.h>
#include <netutil/etharp.h>

#define ETHER_MTU            1500
/// 1500 (MTU) + 14 (Header) + 4(FCS) + 4 (VLAN Tag) + 4 (QinQ Tag) => round to 256
#define ETHER_MAX_SIZE       1536

typedef struct ethernet_state {
    mac_addr mac;
    struct net_device* device;
    struct arp_state* arp;
    struct ip_state* ip;
} Ethernet;

errval_t ethernet_init(
    struct net_device *device, Ethernet* ether
);

errval_t ethernet_marshal(
    Ethernet* ether, mac_addr dst_mac, uint16_t type, uint8_t* data, size_t size
);

errval_t ethernet_unmarshal(
    Ethernet* ether, uint8_t* data, size_t size
);

#endif //__VNET_ETHERNET_H__