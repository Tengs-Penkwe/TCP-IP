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

    // 1.2 Message Queue for single-thread handling of TCP
    for (size_t i = 0; i < IP_SEG_QUEUE_NUMBER; i++)
    {
        BQelem * queue_elements = calloc(IP_SEG_QUEUE_SIZE, sizeof(BQelem));
        err = bdqueue_init(&ip->msg_queue[i], queue_elements, IP_SEG_QUEUE_SIZE);
        if (err_is_fail(err)) {
            free(queue_elements);
            IP_ERR("Can't Initialize the queues for TCP messages");
            return SYS_ERR_INIT_FAIL;
        }
        // 2.1 spinlock for eqch queue
        atomic_flag_clear(&ip->que_locks[i]);
    }
    ip->queue_num  = IP_SEG_QUEUE_NUMBER;
    ip->queue_size = IP_SEG_QUEUE_SIZE;

    TCP_NOTE("TCP Module Initialized, the hash-table for server has size %d, there are %d message "
             "queue, each have %d as maximum size",
             TCP_SERVER_BUCKETS, TCP_QUEUE_NUMBER, TCP_QUEUE_SIZE);

    // 2. ICMP (Internet Control Message Protocol )
    ip->icmp = calloc(1, sizeof(ICMP));
    assert(ip->icmp);
    err = icmp_init(ip->icmp, ip);
    DEBUG_FAIL_RETURN(err, "Can't initialize global ICMP state");

    // 3. UDP (User Datagram Protocol)
    ip->udp = aligned_alloc(ATOMIC_ISOLATION, sizeof(UDP)); 
    assert(ip->udp); memset(ip->udp, 0x00, sizeof(UDP));
    err = udp_init(ip->udp, ip);
    DEBUG_FAIL_RETURN(err, "Can't initialize global UDP state");

    // 4. TCP (Transmission Control Protocol)
    ip->tcp = aligned_alloc(ATOMIC_ISOLATION, sizeof(TCP));
    assert(ip->tcp); memset(ip->tcp, 0x00, sizeof(TCP));
    err = tcp_init(ip->tcp, ip);
    DEBUG_FAIL_RETURN(err, "Can't initialize global TCP state");

    IP_INFO("IP Module initialized");
    return SYS_ERR_OK;
}

void ip_destroy(
    IP* ip
) {
    assert(ip);
    
    free(ip);
    IP_ERR("NYI");
}

errval_t ip_assemble(
    IP* ip, ip_addr_t src_ip, uint8_t proto, uint16_t id, Buffer buf, uint16_t offset, bool more_frag, bool no_frag
) {
    errval_t err;
    assert(ip);
    IP_DEBUG("Assembling a message, ID: %d, size: %d, offset: %d, no_frag: %d, more_frag: %d", id, buf.valid_size, offset, no_frag, more_frag);

    IP_recv *msg = malloc(sizeof(IP_recv)); assert(msg);
    *msg = (IP_recv) {
        .ip     = ip,
        .proto  = proto,
        .id     = id,
        .src_ip = src_ip,
        .buf    = buf,
    };

    if (no_frag == true
        || (more_frag == false && offset == 0)) {  // Which means this isn't a segmented packet
                                                   
        assert(offset == 0 && more_frag == false);
        
        err = ip_handle(msg);
        free(msg);
        DEBUG_FAIL_RETURN(err, "Can't handle this IP message ?");
        return err;
    }

    ip_msg_key_t key = ip_message_hash(src_ip, id);
    
    err = enbdqueue(&ip->msg_queue[key], NULL, msg);
    if (err_is_fail(err)) {
        assert(err_no(err) == EVENT_ENQUEUE_FULL);
        IP_WARN("Too much IP segmentation message for bucket %d, will drop it in upper module", key);
        return err;
    } else {
        return NET_OK_IPv4_SEG_LATER_FREE;
    }
}

errval_t ip_unmarshal(
    IP* ip, Buffer buf
) {
    assert(ip);
    errval_t err;
    struct ip_hdr* packet = (struct ip_hdr*)buf.data;
    
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
    if (ntohs(packet->len) != buf.valid_size) {
        LOG_ERR("IP Packet Size Unmatch %p v.s. %p", ntohs(packet->len), buf.valid_size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (buf.valid_size < IP_LEN_MIN) {
        LOG_ERR("IPv4 Packet too Small: %d", buf.valid_size);
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
    err = arp_lookup_mac(ip->arp, src_ip, &src_mac);
    if (err_no(err) == NET_ERR_ARP_NO_MAC_ADDRESS) {
        USER_PANIC_ERR(err, "You received a message, but you don't know the IP-MAC pair ?");
    } else
        DEBUG_FAIL_RETURN(err, "Can't find binding for given IP address");

    buffer_add_ptr(&buf, header_size);

    // 3. Assemble the IP message
    uint8_t proto = packet->proto;
    err = ip_assemble(ip, src_ip, proto, identification, buf, offset, flag_more_frag, flag_no_frag);
    DEBUG_FAIL_RETURN(err, "Can't assemble the IP message from the packet");

    // 3.1 TTL: TODO, should we deal with it ?
    (void) packet->ttl;

    return err;
}

errval_t ip_marshal(    
    IP* ip, ip_addr_t dst_ip, uint8_t proto, Buffer buf
) {
    errval_t err;
    IP_DEBUG("Sending a message, dst_ip: 0x%0.8X", dst_ip);
    assert(ip);

    // 1. Assign ID
    uint16_t id = (uint16_t) atomic_fetch_add(&ip->seg_count, 1);

    // 2. Create the message
    IP_send *msg = calloc(1, sizeof(IP_send)); assert(msg);
    *msg = (IP_send) {
        .ip             = ip,
        .dst_ip         = dst_ip,
        .dst_mac        = MAC_NULL,
        .proto          = proto,
        .id             = id,
        .buf            = buf,
        .sent_size      = 0,
        .retry_interval = IP_RETRY_SEND_US,
    };

    // 3. Get destination MAC
    mac_addr dst_mac =  MAC_NULL;
    err = arp_lookup_mac(ip->arp, dst_ip, &dst_mac);
    switch (err_no(err))
    {
    case NET_ERR_ARP_NO_MAC_ADDRESS:   // Get Address first
    {
        msg->retry_interval = ARP_WAIT_US;
        submit_delayed_task(MK_DELAY_TASK(msg->retry_interval, close_sending_message, MK_TASK(check_get_mac, (void*)msg)));
        return NET_OK_SUBMIT_EVENT;
    }
    case SYS_ERR_OK: { // Continue sending
        assert(!(maccmp(dst_mac, MAC_NULL) || maccmp(dst_mac, MAC_BROADCAST)));
        msg->dst_mac = dst_mac;
        
        check_send_message((void*)msg);
        return NET_OK_SUBMIT_EVENT;
    }
    default: 
        DEBUG_ERR(err, "Can't establish binding for given IP address");
        return err;
    }
}