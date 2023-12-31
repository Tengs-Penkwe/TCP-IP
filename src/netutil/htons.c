#include <netutil/htons.h>

/**
 * Convert an uint16_t from host- to network byte order.
 *
 * @param n uint16_t in host byte order
 * @return n in network byte order
 */
uint16_t lwip_htons(uint16_t n)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return ((n & 0xff) << 8) | ((n & 0xff00) >> 8);
#else
  return n;
#endif
}

/**
 * Convert an uint16_t from network- to host byte order.
 *
 * @param n uint16_t in network byte order
 * @return n in host byte order
 */
uint16_t lwip_ntohs(uint16_t n)
{
  return lwip_htons(n);
}

/**
 * Convert an uint32_t from host- to network byte order.
 *
 * @param n uint32_t in host byte order
 * @return n in network byte order
 */
uint32_t lwip_htonl(uint32_t n)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return ((n & 0xff) << 24) |
    ((n & 0xff00) << 8) |
    ((n & 0xff0000UL) >> 8) |
    ((n & 0xff000000UL) >> 24);
#else
  return n;
#endif
}

/**
 * Convert an uint32_t from network- to host byte order.
 *
 * @param n uint32_t in network byte order
 * @return n in host byte order
 */
uint32_t lwip_ntohl(uint32_t n)
{
  return lwip_htonl(n);
}

// since mac_addr is a byte array, it's not different between host and network byte order
struct eth_addr hton6(mac_addr to) {
  return to;
}

struct eth_addr ntoh6(mac_addr from) {
  return hton6(from);
}

inline ipv6_addr_t hton16(ipv6_addr_t ip) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return 
  ((ip & (ipv6_addr_t)0x00000000000000FF) << 120)|
  ((ip & (ipv6_addr_t)0x000000000000FF00) << 104)|
  ((ip & (ipv6_addr_t)0x0000000000FF0000) << 88) |
  ((ip & (ipv6_addr_t)0x00000000FF000000) << 72) |
  ((ip & (ipv6_addr_t)0x000000FF00000000) << 56) |
  ((ip & (ipv6_addr_t)0x0000FF0000000000) << 40) |
  ((ip & (ipv6_addr_t)0x00FF000000000000) << 24) |
  ((ip & (ipv6_addr_t)0xFF00000000000000) << 8 ) |
  ((ip & (ipv6_addr_t)0x00000000000000FF << 64) >> 8   )|
  ((ip & (ipv6_addr_t)0x000000000000FF00 << 64) >> 24  )|
  ((ip & (ipv6_addr_t)0x0000000000FF0000 << 64) >> 40  )|
  ((ip & (ipv6_addr_t)0x00000000FF000000 << 64) >> 56  )|
  ((ip & (ipv6_addr_t)0x000000FF00000000 << 64) >> 72  )|
  ((ip & (ipv6_addr_t)0x0000FF0000000000 << 64) >> 88  )|
  ((ip & (ipv6_addr_t)0x00FF000000000000 << 64) >> 104 )|
  ((ip & (ipv6_addr_t)0xFF00000000000000 << 64) >> 120 );
#else
  return ip;
#endif
}

inline ipv6_addr_t ntoh16(ipv6_addr_t ip) {
  return hton16(ip);
}
