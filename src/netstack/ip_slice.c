#include <netutil/htons.h>
#include <netutil/checksum.h>
#include <netstack/ip.h>
#include "ip_slice.h"

#include <event/timer.h>
#include <event/threadpool.h>
#include <event/event.h>
#include <event/states.h>   // g_states.mem_pool

void close_sending_message(void* send) {
    IP_send* msg = send; assert(msg);

    if (msg->dst_ip.is_ipv6)
    {
        assert(msg->id == 0 && "IPv6 doesn't need ID");
        assert(msg->sent_size == 0 && "IPv6 doesn't support segmentation");
    }
    else 
    {
        if (msg->sent_size != msg->buf.valid_size) {
            IP_NOTE("Failed sending a IP packet of %d bytes, only sent %d bytes", msg->buf.valid_size, msg->sent_size); 
        }
    }
    // TODO: where can I free it ?
    free_buffer(msg->buf);
    free(msg);
}

void check_get_mac(void* send) {
    IP_VERBOSE("Check if we got the MAC address");
    errval_t err;
    IP_send* msg = send; assert(msg);
    IP* ip = msg->ip;    assert(ip);

    assert(maccmp(msg->dst_mac, MAC_NULL));
    err = lookup_mac(ip, msg->dst_ip, &msg->dst_mac);
    switch (err_no(err))
    {
    case NET_ERR_NO_MAC_ADDRESS:
        msg->retry_interval *= 2;
        if (msg->retry_interval >= IP_GIVEUP_SEND_US) {
            close_sending_message((void*)msg);
            return;
        }

        if (msg->dst_ip.is_ipv6)
        {
            NDP_marshal* request = malloc(sizeof(NDP_marshal));
            *request = (NDP_marshal) {
                .icmp     = ip->icmp,
                .dst_ip   = msg->dst_ip.ipv6,
                .type     = ICMPv6_NSA,
                .code     = 0,
                .buf      = { 0 },
            };
            pool_alloc(g_states.mempool, sizeof(NDP_marshal), &request->buf);
            assert(request->buf.data);
            submit_task(MK_NORM_TASK(event_ndp_marshal, (void*)request));
        }
        else
        {
            ARP_marshal* request = malloc(sizeof(ARP_marshal));
            *request = (ARP_marshal) {
                .arp      = ip->arp,
                .opration = ARP_OP_REQ,
                .dst_ip   = msg->dst_ip.ipv4,
                .dst_mac  = MAC_BROADCAST,
                .buf      = { 0 },
            };
            pool_alloc(g_states.mempool, sizeof(NDP_marshal), &request->buf);
            assert(request->buf.data);
            submit_task(MK_NORM_TASK(event_arp_marshal, (void*)request));
        }

        IP_INFO("Can't find the Corresponding IP address, sent request, retry later in %d ms", msg->retry_interval / 1000);
        submit_delayed_task(MK_DELAY_TASK(msg->retry_interval, close_sending_message, MK_NORM_TASK(check_get_mac, (void*)msg)));
        break;
    case SYS_ERR_OK:
        assert(!maccmp(msg->dst_mac, MAC_NULL));
        
        // Begin sending message
        msg->retry_interval = IP_RETRY_SEND_US;
        assert(msg->id == 0);  // It should be the first message in this binding since it requires MAC address

        submit_delayed_task(MK_DELAY_TASK(msg->retry_interval, close_sending_message, MK_NORM_TASK(check_send_message, (void*)msg)));
        break;
    default: USER_PANIC_ERR(err, "Unknown sitation");
    }

    IP_VERBOSE("Exit check bind");
    return;
}

void check_send_message(void* send) {
    IP_VERBOSE("Check sending a message");
    errval_t err;
    IP_send* msg = send; assert(msg);
    IP* ip = msg->ip;    assert(ip);

    if (msg->retry_interval > IP_GIVEUP_SEND_US) {
        close_sending_message((void *)msg);
        return;
    }

    if (msg->dst_ip.is_ipv6)
    {
        err = ipv6_send(ip, msg->dst_ip.ipv6, msg->dst_mac, msg->proto, msg->buf);
        if (err_is_fail(err)) {
            msg->retry_interval *= 2;
            DEBUG_ERR(err, "Failed sending an IPv6 packet, will try in %d milliseconds !",
                      msg->retry_interval / 1000);
        } else {
            assert(!err_is_throw(err));
            return;
        }
    }
    else
    {
        err = ipv4_slice(msg);
        if (err_is_fail(err)) {
            msg->retry_interval *= 2;
            DEBUG_ERR(err, "Failed sending an IPv6 packet, will try in %d milliseconds !",
                      msg->retry_interval / 1000);
        }
        if (msg->sent_size == msg->buf.valid_size) { // We are done sending !
            assert(err_no(err) == SYS_ERR_OK);
            IP_DEBUG("We done sending an IP message ! size: %d, retry interval in ms: %d", msg->buf.valid_size, msg->retry_interval / 1000);
            close_sending_message((void*)msg);
            return;
        }
    }

    submit_delayed_task(MK_DELAY_TASK(msg->retry_interval, close_sending_message, MK_NORM_TASK(check_send_message, (void*)msg)));

    IP_VERBOSE("Done Checking a sending message, ttl: %d us, whole size: %d, snet size: %d", msg->retry_interval / 1000, msg->buf.valid_size, msg->sent_size);
    return;
}


/// Assumption: sending is serial, there is only 1 thread sending a slice at one time
///             NO CONCURRENT sending, different slices maybe sent by different thread, but there is no contention
/// @return error code, depends on user if he/she want to retry
errval_t ipv4_send(
    IP* ip, const ip_addr_t dst_ip, const mac_addr dst_mac,
    const uint16_t id, const uint8_t proto,
    Buffer buf,
    const uint16_t send_from, const uint16_t size_to_send,
    bool last_slice
) {
    errval_t err; assert(ip);
    
    // 1. Calculate the information of segmentation
    assert(send_from % 8 == 0);
    uint16_t offset = send_from / 8;
    uint16_t flag_offset = offset & IP_OFFMASK;

    // Reserved Field should be 0
    OFFSET_RF_SET(flag_offset, 0);

    uint16_t pkt_size = size_to_send + sizeof(struct ip_hdr);
    // if the packet is less than 576 byte, we set the non-fragment flag
    bool no_frag = (pkt_size <= IP_MINIMUM_NO_FRAG) && (offset == 0);
    OFFSET_DF_SET(flag_offset, no_frag);

    // More Fragment Field should be 0 for last fragementation
    if (last_slice) OFFSET_MF_SET(flag_offset, 0);    
    else            OFFSET_MF_SET(flag_offset, 1);

    Buffer send_buf = buffer_add(buf, send_from);
    send_buf.valid_size = size_to_send;
    
    // 2. Prepare the send buffer
    buffer_sub_ptr(&send_buf, sizeof(struct ip_hdr));
    /// ALARM: This will destroy the segement before, but since we have sent them, it's ok

    // 3. Fill the header
    struct ip_hdr *packet = (struct ip_hdr*)send_buf.data;
    *packet = (struct ip_hdr) {
        .ihl       = 0x5 ,
        .version   = 0x4,
        .tos       = 0x00,
        .total_len = htons(pkt_size),
        .id        = htons(id),
        .offset    = htons(flag_offset),
        .ttl       = 0xFF,
        .proto     = proto,
        .chksum    = 0,
        .src       = htonl(ip->my_ipv4),
        .dest      = htonl(dst_ip),
    };
    packet->chksum = inet_checksum_in_net_order(packet, sizeof(struct ip_hdr));

    // 4. Send the packet
    err = ethernet_marshal(ip->ether, dst_mac, ETH_TYPE_IPv4, send_buf);
    DEBUG_FAIL_RETURN(err, "Can't send the IPv4 packet");

    IP_VERBOSE("End sending an IP packet with size: %d, offset: %d, no_frag: %d, more_frag: %d, proto: %d, id: %d, src: %0.8X, dst: %0.8X",
            pkt_size, offset * 8, no_frag, !last_slice, proto, id, ip->my_ipv4, dst_ip);
    return SYS_ERR_OK;
}

errval_t ipv4_slice(IP_send* msg) {
    errval_t err;       assert(msg);
    IP* ip = msg->ip;   assert(ip);
    assert(!msg->dst_ip.is_ipv6);

    IP_VERBOSE("Sending IP Message: Protocal %x, whole size: %d, sent size: %d, retry in %d ms",
                msg->proto, msg->buf.valid_size, msg->sent_size, msg->retry_interval / 1000);

    // 1. Get the destination MAC, IP and message ID
    const uint16_t  whole_size = msg->buf.valid_size;
    const uint16_t  sent_size  = msg->sent_size;
    assert(sent_size < whole_size);
    assert(sent_size % 8 == 0);
    assert(msg->buf.from_hdr >= IP_HEADER_RESERVE);

    // 2. Marshal and send sliceS
    for (int size_left = (int)(whole_size - sent_size); size_left > 0; size_left -= IP_MTU) {

        // 2.1 Calculate packet size
        bool last_slice         = (size_left < (int)IP_MTU);
        const uint16_t seg_size = last_slice ? (uint16_t)size_left : IP_MTU;

        err = ipv4_send(ip, msg->dst_ip.ipv4, msg->dst_mac, msg->id, msg->proto,
                    msg->buf, msg->sent_size, seg_size, last_slice);
        if (err_is_fail(err)) {
            IP_INFO("Sending a segment failed, will try latter in %d ms", msg->retry_interval / 1000);
            return err;
        }
        // ALARM: single thread sending
        msg->sent_size += seg_size;
    }

    return SYS_ERR_OK;
}