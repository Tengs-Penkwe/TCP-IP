#include <netstack/tcp.h>
#include <netstack/ip.h>
#include <netutil/checksum.h>
#include <netutil/htons.h>
#include "tcp_server.h"

errval_t tcp_init(TCP* tcp, IP* ip) {
    errval_t err;
    assert(tcp && ip);
    tcp->ip = ip;

    // 1. Hash table for servers
    err = hash_init(&tcp->servers, tcp->buckets, TCP_SERVER_BUCKETS, HS_FAIL_ON_EXIST);
    PUSH_ERR_PRINT(err, SYS_ERR_INIT_FAIL, "Can't initialize the hash table of tcp servers");
    
    // 2. Message Queue for single-thread handlin
    for (size_t i = 0; i < TCP_QUEUE_NUMBER; i++)
    {
        BQelem * queue_elements = calloc(TCP_QUEUE_SIZE, sizeof(BQelem));
        err = bdqueue_init(&tcp->msg_queue[i], queue_elements, TCP_QUEUE_SIZE);
        if (err_is_fail(err)) {
            free(queue_elements);
            hash_destroy(&tcp->servers);
            TCP_ERR("Can't Initialize the queues for TCP messages");
            return SYS_ERR_INIT_FAIL;
        }
        // 2.1 spinlock for eqch queue
        atomic_flag_clear(&tcp->que_locks[i]);
    }
    tcp->queue_num = TCP_QUEUE_NUMBER;

    TCP_NOTE("TCP Module Initialized, the hash-table for server has size %d, there are %d message "
             "queue, each have %d as maximum size",
             TCP_SERVER_BUCKETS, TCP_QUEUE_NUMBER, TCP_QUEUE_SIZE);

    return SYS_ERR_OK;
}

errval_t tcp_marshal(
    TCP* tcp, const ip_addr_t dst_ip, const tcp_port_t src_port, const tcp_port_t dst_port,
    uint32_t seqno, uint32_t ackno, uint32_t window, uint16_t urg_prt, uint8_t flags,
    uint8_t* addr, size_t size
) {
    errval_t err;
    assert(tcp && addr);

    addr -= sizeof(struct tcp_hdr);
    size += sizeof(struct tcp_hdr);

    uint8_t data_offset = TCPH_SET_LEN(sizeof(struct tcp_hdr));

    struct tcp_hdr* packet = (struct tcp_hdr*)addr;
    *packet = (struct tcp_hdr) {
        .src_port    = htons(src_port),
        .dest_port   = htons(dst_port),
        .seqno       = htonl(seqno),
        .ackno       = htonl(ackno),
        .data_offset = data_offset,
        .flags       = flags,
        .window      = htons(window),
        .chksum      = 0,
        .urgent_ptr  = urg_prt,
    };

    struct pseudo_ip_header_in_net_order ip_header = {
        .src_addr       = htonl(tcp->ip->my_ip),
        .dst_addr       = htonl(dst_ip),
        .reserved       = 0,
        .protocol       = IP_PROTO_TCP,
        .len_no_iph     = htonl(size),
    };
    packet->chksum  = tcp_udp_checksum_in_net_order(addr, ip_header);

    err = ip_marshal(tcp->ip, dst_ip, IP_PROTO_TCP, addr, size);
    RETURN_ERR_PRINT(err, "Can't marshal the TCP packet and sent by IP");

    return SYS_ERR_OK;
}

static size_t queue_hash(ip_addr_t src_ip, tcp_port_t src_port, tcp_port_t dst_port) {
    // TODO: use a better hash function
    return ((size_t)src_ip + (size_t)src_port + (size_t)dst_port) % TCP_QUEUE_NUMBER;
}

errval_t tcp_unmarshal(
    TCP* tcp, const ip_addr_t src_ip, uint8_t* addr, uint16_t size
) {
    assert(tcp);
    errval_t err;
    struct tcp_hdr* packet = (struct tcp_hdr*)addr;

    // 0. Check Validity
    uint8_t reserved = TCP_RSVR(packet);
    if (reserved != 0x00) {
        TCP_ERR("The TCP reserved field 0x%02x should be 0 !", reserved);
        return NET_ERR_TCP_WRONG_FIELD;
    }
    uint8_t offset = TCP_HLEN(packet);
    if (!(offset >= TCP_HLEN_MIN && offset <= TCP_HLEN_MAX)) {
        TCP_ERR("The TCP header size: %d is invalid", offset);
        return NET_ERR_TCP_WRONG_FIELD;
    }

    // 1. Find the Connection
    tcp_port_t src_port = ntohs(packet->src_port);
    tcp_port_t dst_port = ntohs(packet->dest_port);
    
    // 2. Checksum
    struct pseudo_ip_header_in_net_order ip_header = {
        .src_addr       = htons(src_ip),
        .dst_addr       = htons(tcp->ip->my_ip),
        .reserved       = 0,
        .protocol       = IP_PROTO_TCP,
        .len_no_iph     = htons(size),
    };
    uint16_t chksum = ntohs(packet->chksum);
    packet->chksum  = 0;
    uint16_t tcp_chksum = ntohs(tcp_udp_checksum_in_net_order(addr, ip_header));
    if (chksum != tcp_chksum) {
        TCP_ERR("The TCP checksum %p should be %p", chksum, tcp_chksum);
        return NET_ERR_TCP_WRONG_FIELD;
    }

    // 2. Create the Message
    uint32_t seqno = ntohl(packet->seqno);
    uint32_t ackno = ntohl(packet->ackno);

    // We ignore it for now
    uint32_t window = ntohs(packet->window);
    (void) window;

    uint8_t flags = packet->flags;
    TCP_msg* msg = calloc(1, sizeof(TCP_msg));
    assert(msg);
    *msg = (TCP_msg) {
        .seqno    = seqno,
        .ackno    = ackno,
        .data     = addr + offset,
        .size     = size - offset,
        .flags    = get_tcp_flags(flags),
        .recv     = {
            .src_ip   = src_ip,
            .src_port = src_port,
        }
    };
    
    size_t hash = queue_hash(src_ip, src_port, dst_port);
    assert(hash <= tcp->queue_num);
    
    err = enbdqueue(&tcp->msg_queue[hash], NULL, (void *)msg);
    if (err_is_fail(err)) {
        assert(err_no(err) == EVENT_ENQUEUE_FULL);
        TCP_ERR("The given message queue of TCP message is full, will drop this message in upper level");
        return err_push(err, NET_ERR_TCP_QUEUE_FULL);
    }

    return SYS_ERR_OK;
}
