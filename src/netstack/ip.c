#include <netutil/ip.h>
#include <netutil/htons.h>
#include <netutil/checksum.h>
#include <netutil/dump.h>
#include <netstack/ip.h>
#include <event/timer.h>
#include <event/threadpool.h>

/* TODO: Consider these situations
1. two workers received same segment at the same time
2. two workers handle the duplicate last segments at the same time
3. A modifies the hash table whill B is reading it
4. A reads when the hash table is resizing
*/

#include "kavl-lite.h"  // AVL tree for segmentation

// Forward declaration
// static void check_get_mac(void* message);
// static void check_send_message(void* message);
static errval_t ip_handle(IP_message* msg);

/***************************************************
* Structure to deal with IP segmentation
* Shoud be private to user
**************************************************/
#define SIZE_DONT_KNOW  0xFFFFFFFF

typedef struct message_segment Mseg;
typedef struct message_segment {
    uint32_t offset;
    KAVLL_HEAD(Mseg) head;
} Mseg ;

#define seg_cmp(p, q) (((q)->offset < (p)->offset) - ((p)->offset < (q)->offset))

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
KAVLL_INIT(Mseg, Mseg, head, seg_cmp)
#pragma GCC diagnostic pop

/// @brief Presentation of an IP Message
typedef struct ip_message {
    struct ip_state *ip;     ///< Global IP state

    uint8_t          proto;  ///< Protocal over IP
    uint16_t         id;     ///< Message ID

    pthread_mutex_t  mutex;

    uint32_t         whole_size;  ///< Size of the whole message
    /// alloc_size == 0 have special meaning, it's used in close_message
    uint32_t         alloc_size;  ///< Record how much space does the data pointer holds
    uint8_t         *data;        ///< Holds all the data
    Mseg            *seg;         ///< All the offset of segments we received yet

    union {
        struct {
            uint32_t  size;      ///< How many bytes have we received
            int       times_to_live;
            ip_addr_t src_ip;
        } recvd ;
        struct {        
            uint32_t  size;      ///< How many bytes have we sent
            int       retry_interval;
            ip_addr_t dst_ip;
            mac_addr  dst_mac;
        } sent;
    };
} IP_message;

static inline void close_message(void* message) {
    IP_message* msg = message; assert(msg);
    IP* ip = msg->ip; assert(ip);

    ip_msg_key_t msg_key = MSG_KEY(msg->recvd.src_ip, msg->id);
    khint64_t key = kh_get(ip_msg, ip->recv_messages, msg_key);

    if (key == kh_end(ip->recv_messages) && msg->alloc_size != 0)
        USER_PANIC("The message doesn't exist in hash table before we delete it!");

    if (msg->alloc_size != 0) { // Means there was assembling process
        // Delete the message from the hash table
        pthread_mutex_lock(&ip->recv_mutex);
        kh_del(ip_msg, ip->recv_messages, msg_key);
        pthread_mutex_unlock(&ip->recv_mutex);
        IP_NOTE("Deleted a message from hash table");

        // Free all segments
        assert(msg->seg);
        kavll_free(Mseg, head, msg->seg, free);
        msg->seg = NULL;

        // Note: it doesn't matter if we use recvd.size or sent.size since we used the union
        assert(msg->whole_size && msg->recvd.size);
        // Free the allocated data:
        free(msg->data);
        // if message come here directly, data should be free'd in lower module

        // Destroy mutex
        pthread_mutex_destroy(&msg->mutex);
    }

    assert(msg->seg == NULL);
    // Free the message itself and set it to NULL
    free(message);
    message = NULL;
}

static errval_t mac_lookup(
    IP* ip, ip_addr_t dst_ip, mac_addr* dst_mac
) {
    errval_t err;
    assert(ip);

    err = arp_lookup_mac(ip->arp, dst_ip, dst_mac);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Can't get the corresponding MAC address of IP address");
        errval_t error = arp_send(ip->arp, ARP_OP_REQ, dst_ip, MAC_BROADCAST);
        if (err_is_fail(error)) 
            DEBUG_ERR(error, "I Can't even send an ARP request after I can't find the MAC for given IP");
        return err;
    }

    assert(!(maccmp(*dst_mac, MAC_NULL) || maccmp(*dst_mac, MAC_BROADCAST)));

    return SYS_ERR_OK;
}

errval_t ip_init(
    IP* ip, Ethernet* ether, ARP* arp, ip_addr_t my_ip
) {
    errval_t err;
    assert(ip && ether && arp);

    ip->ip = my_ip;
    ip->ether = ether;
    ip->arp = arp;

    ip->seg_count = 0;

    if (pthread_mutex_init(&ip->recv_mutex, NULL) != 0) {
        ARP_ERR("Can't initialize the mutex for IP");
        return SYS_ERR_FAIL;
    }
    
    ip->recv_messages = kh_init(ip_msg);
    ip->send_messages = kh_init(ip_msg);

    ip->icmp = calloc(1, sizeof(ICMP));
    assert(ip->icmp);
    err = icmp_init(ip->icmp, ip);
    RETURN_ERR_PRINT(err, "Can't initialize global ICMP state");

    // ip->udp = calloc(1, sizeof(UDP));
    // assert(ip->udp);
    // err = udp_init(ip->udp, ip);
    // RETURN_ERR_PRINT(err, "Can't initialize global UDP state");

    // ip->tcp = calloc(1, sizeof(TCP));
    // assert(ip->tcp);
    // err = tcp_init(ip->tcp, ip);
    // RETURN_ERR_PRINT(err, "Can't initialize global TCP state");

    IP_INFO("IP Module initialized");
    return SYS_ERR_OK;
}

/**
 * @brief Checks the status of an IP message, drops it if TTL expired, or processes it if complete.
 *        This functions is called in a periodic event
 * 
 * @param message Pointer to the IP_message structure to be checked.
 */
static void check_recvd_message(void* message) {
    IP_VERBOSE("Checking a message");
    errval_t err;
    IP_message* msg = message; assert(msg);

    /// Also modified in ip_assemble(), be careful of global states
    msg->recvd.times_to_live *= 1.5;
    if (msg->recvd.times_to_live >= IP_GIVEUP_RECV_US)
    {
        close_message(msg);
    }
    else
    {
        if (msg->recvd.size == msg->whole_size) { // We can process the package now
            // We don't need to care about duplicate segment here, they are deal in ip_assemble
            IP_DEBUG("We spliced an IP message of size %d, ttl: %d, now let's process it", msg->whole_size, msg->recvd.times_to_live);

            err = ip_handle(msg);
            if (err_is_fail(err)) { DEBUG_ERR(err, "We meet an error when handling an IP message"); }
            close_message(msg);
        }
        else
        {
            submit_delayed_task(msg->recvd.times_to_live, MK_TASK(check_recvd_message, (void *)msg));
            IP_VERBOSE("Done Checking a message, ttl: %d, whole size: %p", msg->recvd.times_to_live, msg->whole_size);
        }
    }
}

/**
 * @brief Assemble an IP packet to message
 *
 * @param[in]   bind        IP_binding struct stores all the states
 * @param[in]   id          Identification number of the message.
 * @param[in]   msg_len     Length of the whole message
 * @param[in]   data        The start of the bufffer to copy from
 * @param[in]   size        Length of this packet
 *
 * @return Returns error code indicating success or failure.
 */
static errval_t ip_assemble(
    IP* ip, ip_addr_t src_ip, uint8_t proto, uint16_t id, uint8_t* addr, size_t size, uint32_t offset, bool more_frag, bool no_frag
) {
    // errval_t err;
    assert(ip && addr);
    IP_DEBUG("Assembling a message, ID: %d, addr: %p, size: %d, offset: %d, no_frag: %d, more_frag: %d", id, addr, size, offset, no_frag, more_frag);

    ip_msg_key_t msg_key = MSG_KEY(src_ip, id);
    IP_message* msg = NULL;

    ///TODO: add lock ?
    khint64_t key = kh_get(ip_msg, ip->recv_messages, msg_key);
    // Try to find if it already exists
    if (key == kh_end(ip->recv_messages)) {  // This message doesn't exist yet
        msg = malloc(sizeof(IP_message)); assert(msg);
        *msg = (IP_message) {
            .ip         = ip,
            .proto      = proto,
            .id         = id,
            .mutex      = { { 0 } },
            .whole_size = SIZE_DONT_KNOW,     // We don't know the size util the last packet arrives
            .alloc_size = 0,
            .seg        = NULL,
            .data       = NULL,
            .recvd      = {
                .size   = size,
                .times_to_live = IP_RETRY_RECV_US,
                .src_ip = src_ip,
            },
        };

        ///TODO: What if 2 threads received duplicate segmentation at the same time ?
        if (no_frag == true || (more_frag == false && offset == 0)) { // Which means this isn't a segmented packet
            assert(offset == 0 && more_frag == false);

            msg->data = addr;
            assert(msg->alloc_size == 0);
            assert(msg->whole_size == SIZE_DONT_KNOW);
            msg->whole_size = size;
            check_recvd_message((void*) msg);

            return SYS_ERR_OK;
        }

        // Multiple threads may handle different segmentation of IP at same time, need to deal with it 
        if (pthread_mutex_init(&msg->mutex, NULL) != 0) {
            IP_ERR("Can't initialize the mutex for IP message");
            return SYS_ERR_INIT_FAIL;
        }

        // Root of AVL tree
        msg->seg = malloc(sizeof(Mseg));  assert(msg->seg);
        msg->seg->offset = offset;

        ///TODO: Should I lock it before kh_get, if thread A is changing the hash table while thread
        // B is reading it, will it cause problem ? 
        pthread_mutex_lock(&ip->recv_mutex);
        int ret;
        key = kh_put(ip_msg, ip->recv_messages, msg_key, &ret); 
        switch (ret) {
        case -1:    // The operation failed
            pthread_mutex_unlock(&ip->recv_mutex);
            USER_PANIC("Can't add a new message with seqno: %d to hash table", id);
        case 1:     // the bucket is empty 
        case 2:     // the element in the bucket has been deleted 
        case 0: 
            break;
        default:    USER_PANIC("Can't be this case: %d", ret);
        }
        // Set the value of key
        kh_value(ip->recv_messages, key) = msg;
        pthread_mutex_unlock(&ip->recv_mutex);
        
    } else {
        // TODO: should I accquire the lock ?
        msg = kh_val(ip->recv_messages, key); assert(msg);
        assert(msg->proto == proto);
        ///ALARM: global state, also modified in check_recvd_message
        msg->recvd.times_to_live /= 1.5;  // We got 1 more packet, wait less time

        Mseg* seg = malloc(sizeof(Mseg)); assert(seg);
        seg->offset = offset;
        if (seg != Mseg_insert(&msg->seg, seg)) { // Already exists !
            IP_ERR("We have duplicate IP message segmentation with offset: %d", seg->offset);
            dump_ipv4_header((const struct ip_hdr*) (addr - sizeof(struct ip_hdr)));
            free(seg);
            return NET_ERR_IPv4_DUPLITCATE_SEG;
        }
    }

    uint32_t needed_size = offset + size;
    // If this the laset packet, we now know the final size
    if (more_frag == false) {
        assert(no_frag == false);
        assert(msg->whole_size == SIZE_DONT_KNOW);
        msg->whole_size = offset + size;
    }
    // If the current size is smaller, we need to re-allocate space
    if (msg->alloc_size < needed_size) {
        msg->alloc_size = needed_size;
        if (msg->data == NULL) {
            msg->data = malloc(msg->alloc_size); assert(msg->data);
        } else {
            msg->data = realloc(msg->data, needed_size); assert(msg->data);
        }
    }
    // Copy the message to the buffer
    memcpy(msg->data + offset, (void*)addr, size);
    msg->recvd.size += size; 
    IP_VERBOSE("Done Assembling a packet of a message");

    // If the message is complete, we trigger the check message,
    if (msg->recvd.size == msg->whole_size) {
        IP_ERR("Completed");
        check_recvd_message((void*) msg);
    } else {
        IP_WARN("Later");
        submit_delayed_task(msg->recvd.times_to_live, MK_TASK(check_recvd_message, (void*)msg));
    }
    
    return SYS_ERR_OK;
}

errval_t ip_unmarshal(
    IP* ip, uint8_t* data, size_t size
) {
    assert(ip && data);
    errval_t err;
    struct ip_hdr* packet = (struct ip_hdr*)data;
    
    /// 1. Decide if the packet is correct
    if (IPH_V(packet) != 4) {
        LOG_ERR("IP Protocal Version Mismatch");
        return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (packet->tos != 0x00) {
        LOG_ERR("We Don't Support TOS Field: %p, But I'll Ignore it for Now", packet->tos);
        // return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (IPH_HL(packet) != sizeof(struct ip_hdr)) {
        LOG_ERR("We Only Support IP Header Length as 20 Bytes");
        return NET_ERR_IPv4_WRONG_FIELD;
    }

    // 1.2 Packet Size check
    if (ntohs(packet->len) != size) {
        LOG_ERR("IP Packet Size Unmatch %p v.s. %p", ntohs(packet->len), size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }
    if (!(size > IP_LEN_MIN && size < IP_LEN_MAX)) {
        LOG_ERR("IPv4 Packet to Big or Small: %d", size);
        return NET_ERR_IPv4_WRONG_FIELD;
    }

    // 1.3 Checksum
    uint16_t packet_checksum = ntohs(packet->chksum);
    packet->chksum = 0;     // Set the it as 0 to calculate
    uint16_t checksum = inet_checksum(packet, sizeof(struct ip_hdr));
    if (packet_checksum != ntohs(checksum)) {
        LOG_ERR("This IPv4 Pacekt Has Wrong Checksum %p, Should be %p", checksum, packet_checksum);
        return NET_ERR_IPv4_WRONG_CHECKSUM;
    }

    // 1.4 Destination IP
    ip_addr_t dst_ip = ntohl(packet->dest);
    if (dst_ip != ip->ip) {
        LOG_ERR("This IPv4 Pacekt isn't for us %p but for %p", ip->ip, dst_ip);
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
    err = mac_lookup(ip, src_ip, &src_mac);
    if (err_no(err) == NET_ERR_ARP_NO_MAC_ADDRESS) {
        USER_PANIC_ERR(err, "You received a message, but you don't know the IP-MAC pair ?");
    } else
        RETURN_ERR_PRINT(err, "Can't find binding for given IP address");

    data += sizeof(struct ip_hdr);
    size -= sizeof(struct ip_hdr);

    // 3. Assemble the IP message
    uint8_t proto = packet->proto;
    err = ip_assemble(ip, src_ip, proto, identification, data, size, offset, flag_more_frag, flag_no_frag);
    RETURN_ERR_PRINT(err, "Can't assemble the IP message from the packet");

    // 3.1 TTL: TODO, should we deal with it ?
    (void) packet->ttl;

    return SYS_ERR_OK;
}

/**
 * @brief Unmarshals a complete IP message and processes it based on its protocol type.
 * 
 * @param msg Pointer to the IP message to be unmarshalled.
 * 
 * @return Returns error code indicating success or failure.
 */
static errval_t ip_handle(IP_message* msg) {
    errval_t err;
    IP_VERBOSE("An IP Message has been assemble, now let's process it");

    switch (msg->proto) {
    case IP_PROTO_ICMP:
        IP_VERBOSE("Received a ICMP packet");
LOG_ERR("msg: %p, seg: %p, data: %p", msg, msg->seg, msg->data);
        err = icmp_unmarshal(msg->ip->icmp, msg->recvd.src_ip, msg->data, msg->recvd.size);
        RETURN_ERR_PRINT(err, "Error when unmarshalling an ICMP message");
LOG_ERR("msg: %p, seg: %p, data: %p", msg, msg->seg, msg->data);
        break;
    case IP_PROTO_UDP:
        IP_VERBOSE("Received a UDP packet");
        // err = udp_unmarshal(msg->ip->udp, msg->recvd.src_ip, msg->, msg->recvd.size);
        // RETURN_ERR_PRINT(err, "Error when unmarshalling an UDP message");
        break;
    case IP_PROTO_IGMP:
    case IP_PROTO_UDPLITE:
        LOG_ERR("Unsupported Protocal");
        return SYS_ERR_NOT_IMPLEMENTED;
    case IP_PROTO_TCP:
        IP_VERBOSE("Received a TCP packet");
        // err = tcp_unmarshal(msg->ip->tcp, msg->recvd.src_ip, msg->, msg->recvd.size);
        // RETURN_ERR_PRINT(err, "Error when unmarshalling an TCP message");
        break;
    default:
        LOG_ERR("Unknown packet type for IPv4: %p", msg->proto);
        return NET_ERR_IPv4_WRONG_PROTOCOL;
    }

    IP_VERBOSE("Done unmarshalling a IPv4 message");
    return SYS_ERR_OK;
}

static errval_t ip_send(
    IP* ip, const ip_addr_t dst_ip, const mac_addr dst_mac,
    const uint16_t id, const uint8_t proto,
    const uint8_t *data, const uint32_t sent_size, const uint32_t whole_size)
{
    errval_t err;

    int size_left = (int)(whole_size - sent_size);

    // 1. Marshal the Ethernet packet
    int offset = sent_size;   assert(offset >= 0 && offset % 8 == 0);
    const size_t seg_size = size_left < (int)IP_MTU ? (size_t)size_left : IP_MTU;
    const size_t pkt_size = seg_size + sizeof(struct ip_hdr);
    offset /= 8;

    uint16_t flag_offset = offset & IP_OFFMASK;
    // Reserved Field should be 0
    OFFSET_RF_SET(flag_offset, 0);
    ///TODO: come with a threashold to set this flag
    OFFSET_DF_SET(flag_offset, 0);
    // More Fragment Field should be 0 for last fragementation
    if (size_left <= (int)IP_MTU) OFFSET_MF_SET(flag_offset, 0);    
    else                          OFFSET_MF_SET(flag_offset, 1);
    
    // 2. Prepare the send buffer
    uint8_t* data_to_send = NULL;
    if (sent_size == 0 && size_left <= (int)IP_MTU) { // Means we don't do copy, send directly
        data_to_send = (uint8_t*)data;
        ///ALARM: should have reserved space before !
        data_to_send -= sizeof(struct ip_hdr);
    } else {
        data_to_send = malloc(pkt_size);
        memcpy(data_to_send + sizeof(struct ip_hdr), data + offset, pkt_size);
    }

    // 3. Fill the header
    struct ip_hdr *packet = (struct ip_hdr*) data_to_send;
    *packet = (struct ip_hdr) {
        .ihl     = 0x5,
        .version = 0x4,
        .tos     = 0x00,
        .len     = htons(pkt_size),
        .id      = htons(id),
        .offset  = htons(flag_offset),
        .ttl     = 0xFF,
        .proto   = proto,
        .chksum  = 0,
        .src     = htonl(ip->ip),
        .dest    = htonl(dst_ip),
    };
    uint16_t checksum = inet_checksum(packet, sizeof(struct ip_hdr));
    packet->chksum = checksum;

    // 4. Send the packet
    err = ethernet_marshal(ip->ether, dst_mac, ETH_TYPE_IPv4, (uint8_t*)packet, pkt_size);
    RETURN_ERR_PRINT(err, "Can't send the IPv4 packet");

    IP_VERBOSE("End sending IP Message");
    return SYS_ERR_OK;
}

errval_t ip_marshal(    
    IP* ip, ip_addr_t dst_ip, uint8_t proto, const uint8_t* data, const size_t size
) {
    errval_t err;
    IP_DEBUG("Sending a message, ip:%p, dst_ip: %p, data: %p, size: %d ", ip, dst_ip, data, size);
    assert(ip && data);

    // 1. Assign ID
    uint16_t id = (uint16_t) atomic_fetch_add(&ip->seg_count, 1);

    // 2. Get destination MAC
    mac_addr dst_mac =  MAC_NULL;
    err = mac_lookup(ip, dst_ip, &dst_mac);
    if (err_no(err) == NET_ERR_ARP_NO_MAC_ADDRESS) {
        USER_PANIC_ERR(err, "TODO: add get MAC logic");
    } else if (err_is_fail(err)) {
        DEBUG_ERR(err, "Can't establish binding for given IP address");
        return err;
    } 
    assert(!maccmp(dst_mac, MAC_NULL));

    if (size < IP_MTU) { // Don't need to create IP_message structure
        err = ip_send(ip, dst_ip, dst_mac, id, proto,
                      data, // ALARM: Should have reserved space before
                      0, size);
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
    // err = mac_lookup(ip, dst_ip, &dst_mac);
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