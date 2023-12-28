#include <netutil/ip.h>
#include <netutil/htons.h>
#include <netutil/checksum.h>
#include <netutil/dump.h>
#include <netstack/ip.h>
#include <event/timer.h>
#include <event/threadpool.h>

#include "ip_gather.h"
#include "ip_slice.h"


errval_t ip_init(
    IP* ip, Ethernet* ether, ARP* arp, ip_addr_t my_ip
) {
    errval_t err;
    assert(ip && ether && arp);

    ip->my_ip = my_ip;
    ip->ether = ether;
    ip->arp = arp;

    ip->seg_count = 0;

    if (pthread_mutex_init(&ip->mutex, NULL) != 0) {
        ARP_ERR("Can't initialize the mutex for IP");
        return SYS_ERR_FAIL;
    }
    
    ip->recv_messages = kh_init(ip_recv);

    // 2. ICMP (Internet Control Message Protocol )
    ip->icmp = calloc(1, sizeof(ICMP));
    assert(ip->icmp);
    err = icmp_init(ip->icmp, ip);
    RETURN_ERR_PRINT(err, "Can't initialize global ICMP state");

    // 3. UDP (User Datagram Protocol)
    ip->udp = aligned_alloc(ATOMIC_ISOLATION, sizeof(UDP));
    memset(ip->udp, 0x00, sizeof(UDP));
    assert(ip->udp);
    err = udp_init(ip->udp, ip);
    RETURN_ERR_PRINT(err, "Can't initialize global UDP state");

    // 4. TCP (Transmission Control Protocol)
    // ip->tcp = calloc(1, sizeof(TCP));
    // assert(ip->tcp);
    // err = tcp_init(ip->tcp, ip);
    // RETURN_ERR_PRINT(err, "Can't initialize global TCP state");

    IP_INFO("IP Module initialized");
    return SYS_ERR_OK;
}

void ip_destroy(
    IP* ip
) {
    assert(ip);
    
    pthread_mutex_destroy(&ip->mutex);
    free(ip);
    
    IP_ERR("NYI");
}

errval_t ip_unmarshal(
    IP* ip, uint8_t* data, uint16_t size
) {
    assert(ip && data);
    errval_t err;
    struct ip_hdr* packet = (struct ip_hdr*)data;
    
    /// 1. Decide if the packet is correct
    if (packet->version != 4) {
        IP_ERR("IP Protocal Version Mismatch");
        return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (packet->tos != 0x00) {
        IP_ERR("We Don't Support TOS Field: %p, But I'll Ignore it for Now", packet->tos);
        // return NET_ERR_IPv4_WRONG_FIELD;
    }

    const uint16_t header_size = IPH_HL(packet);
    if (header_size != sizeof(struct ip_hdr)) {
        IP_NOTE("The IP Header has %d Bytes, We don't have special treatment for it", header_size);
    }
    if (!(header_size >= IPH_LEN_MIN && header_size <= IPH_LEN_MAX)) {
        IP_ERR("IPv4 Header to Big or Small: %d", header_size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }

    // 1.2 Packet Size check
    if (ntohs(packet->len) != size) {
        LOG_ERR("IP Packet Size Unmatch %p v.s. %p", ntohs(packet->len), size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (!(size >= IP_LEN_MIN)) {
        LOG_ERR("IPv4 Packet too Small: %d", size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }

    // 1.3 Checksum
    uint16_t packet_checksum = ntohs(packet->chksum);
    packet->chksum = 0;     // Set the it as 0 to calculate
    uint16_t checksum = inet_checksum(packet, header_size);
    if (packet_checksum != ntohs(checksum)) {
        LOG_ERR("This IPv4 Pacekt Has Wrong Checksum %p, Should be %p", checksum, packet_checksum);
        return NET_ERR_IPv4_WRONG_CHECKSUM;
    }

    // 1.4 Destination IP
    ip_addr_t dst_ip = ntohl(packet->dest);
    if (dst_ip != ip->my_ip) {
        LOG_ERR("This IPv4 Pacekt isn't for us %p but for %p", ip->my_ip, dst_ip);
        return NET_ERR_IPv4_WRONG_IP_ADDRESS;
    }

    // TODO: Cache the IP & MAC in ARP
    // Re-consider it, this may break the layer model ?

    // 2. Fragmentation
    const uint16_t identification = ntohs(packet->id);
    const uint16_t flag_offset = ntohs(packet->offset);
    const bool flag_reserved = flag_offset & IP_RF;
    const bool flag_no_frag  = flag_offset & IP_DF;
    const bool flag_more_frag= flag_offset & IP_MF;
    const uint32_t offset    = (flag_offset & IP_OFFMASK) * 8;
    assert(offset <= 0xFFFF);
    if (flag_reserved || (flag_no_frag && flag_more_frag)) {
        LOG_ERR("Problem with flags, reserved: %d, no_frag: %d, more_frag: %d", flag_reserved, flag_no_frag, flag_more_frag);
        return NET_ERR_IPv4_WRONG_FIELD;
    }

    // 2.1 Find or Create the binding
    ip_addr_t src_ip = ntohl(packet->src);
    mac_addr src_mac = MAC_NULL;
    err = mac_lookup_and_send(ip->arp, src_ip, &src_mac);
    if (err_no(err) == NET_ERR_ARP_NO_MAC_ADDRESS) {
        USER_PANIC_ERR(err, "You received a message, but you don't know the IP-MAC pair ?");
    } else
        RETURN_ERR_PRINT(err, "Can't find binding for given IP address");

    data += header_size;
    size -= header_size;

    // 3. Assemble the IP message
    uint8_t proto = packet->proto;
    err = ip_assemble(ip, src_ip, proto, identification, data, size, offset, flag_more_frag, flag_no_frag);
    RETURN_ERR_PRINT(err, "Can't assemble the IP message from the packet");

    // 3.1 TTL: TODO, should we deal with it ?
    (void) packet->ttl;

    return SYS_ERR_OK;
}

errval_t ip_marshal(    
    IP* ip, ip_addr_t dst_ip, uint8_t proto, const uint8_t* data, const uint16_t size
) {
    errval_t err;
    IP_DEBUG("Sending a message, dst_ip: %p, data: %p, size: %d", dst_ip, data, size);
    assert(ip && data);

    // 1. Assign ID
    uint16_t id = (uint16_t) atomic_fetch_add(&ip->seg_count, 1);

    // 2. Get destination MAC
    mac_addr dst_mac =  MAC_NULL;
    err = mac_lookup_and_send(ip->arp, dst_ip, &dst_mac);
    if (err_no(err) == NET_ERR_ARP_NO_MAC_ADDRESS) {
        USER_PANIC_ERR(err, "TODO: add get MAC logic");
    } else if (err_is_fail(err)) {
        DEBUG_ERR(err, "Can't establish binding for given IP address");
        return err;
    } 
    assert(!maccmp(dst_mac, MAC_NULL));

    if (size < IP_MTU) { // Don't need to create IP_message structure
        err = ip_send(ip, dst_ip, dst_mac, id, proto,
                      data,  // ALARM: Should have reserved space before
                      0, size, true);
        RETURN_ERR_PRINT(err, "Can't send the non-segmented IP packet");
    }

    // // 2. Avoid duplication
    // uint64_t msg_key = MSG_KEY(dst_ip, id);
    // IP_message* msg = collections_hash_find(ip->send_messages, msg_key);
    // assert(msg == NULL);    // Must be null, since we don't have a binding

    // // 2.1 Create the message
    // msg = calloc(1, sizeof(IP_message));
    // assert(msg);
    // *msg = (IP_message) {
    //     .ip         = ip,
    //     .proto      = proto,
    //     .id         = id,
    //     .whole_size = size,
    //     .alloc_size = size,  // Redundant
    //     .data       = data,
    //     .sent       = { 
    //         .size   = 0, 
    //         .retry_interval = IP_RETRY_SEND_US,
    //         .dst_ip  = dst_ip,
    //         .dst_mac = MAC_NULL,
    //     },
    // };
    // assert(msg->data);

    // // 2.2 Add it to the hash table
    // collections_hash_insert(ip->send_messages, msg_key, msg);

    // // 3. Try to find the MAC
    // mac_addr dst_mac =  MAC_NULL;
    // err = mac_lookup_and_send(ip->arp, dst_ip, &dst_mac);
    // if (err_no(err) == NET_ERR_IPv4_NO_MAC_ADDRESS) {

    //     LOG_INFO("We don't have MAC for this IP: %p", dst_ip);
    //     msg->sent.retry_interval = ARP_WAIT_US;

    //     // Register Evenet to check later
    //     err = deferred_event_register(&msg->defer, ip->ws, msg->sent.retry_interval, MKCLOSURE(check_get_mac, (void*)msg));
    //     if (err_is_fail(err)) {
    //         free_message(msg);
    //         USER_PANIC_ERR(err, "We can't register a deferred event to deal with the IP message\n TODO: let's deal with it later");
    //     }
    // } else if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "Can't establish binding for given IP address");
    //     return err;
    // } else {
    //     msg->sent.dst_mac = dst_mac;
    //     check_send_message((void *)msg);
    // }
    return SYS_ERR_OK;
}