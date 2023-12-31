#include <common.h>
#include <netstack/ethernet.h>
#include <event/event.h>

void event_ether_unmarshal(void* unmarshal) {
    
    // TODO: copy the argument to stack, and free the argument
    assert(unmarshal);
    Ether_unmarshal frame = *(Ether_unmarshal*) unmarshal; 
    free(unmarshal);

    errval_t err = ethernet_unmarshal(frame.ether, frame.buf);
    switch (err_no(err))
    {
    case NET_THROW_TCP_ENQUEUE:
    {
        EVENT_INFO("A TCP message is successfully enqueued, Can't free the buffer now");
        break;
    }
    case NET_THROW_SUBMIT_EVENT:
    {
        EVENT_INFO("An Event is submitted, and the buffer is re-used, can't free now");
        break;
    }
    case NET_ERR_TCP_QUEUE_FULL:
    {
        assert(err_pop(err) == EVENT_ENQUEUE_FULL);
        EVENT_WARN("This should be a TCP message that has its queue full, drop it");
        free_buffer(frame.buf);
        break;
    }
    case SYS_ERR_NOT_IMPLEMENTED:
    case NET_ERR_ETHER_WRONG_MAC:
    case NET_ERR_ETHER_NO_MAC:
        free_buffer(frame.buf);
        DEBUG_ERR(err, "A known error happend, the process continue");
        break;
    case SYS_ERR_OK:
        free_buffer(frame.buf);
        break;
    case NET_THROW_IPv4_SEG:
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}

void event_arp_marshal(void* send) {
    errval_t err; assert(send);

    ARP_marshal marshal = *(ARP_marshal*) send;
    free(send);

    err = arp_marshal(marshal.arp, marshal.opration, marshal.dst_ip, marshal.dst_mac, marshal.buf);
    switch (err_no(err))
    {
    case SYS_ERR_OK:
        free_buffer(marshal.buf);
        break;
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}

void event_icmp_marshal(void* send) {
    errval_t err; assert(send);

    ICMP_marshal marshal = *(ICMP_marshal*) send;
    free(send);

    err = icmp_marshal(marshal.icmp, marshal.dst_ip, marshal.type, marshal.code, marshal.field, marshal.buf);
    switch (err_no(err))
    {
    case NET_THROW_SUBMIT_EVENT:
    {
        EVENT_INFO("An Event is submitted, and the buffer is re-used, can't free now");
        break;
    }
    case SYS_ERR_NOT_IMPLEMENTED:
        EVENT_INFO("ICMP type not implemented, free the buffer now");
        __attribute__((fallthrough));
    case SYS_ERR_OK:
        free_buffer(marshal.buf);
        break;
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}

void event_ip_assemble(void* recvd_segment) {
    errval_t err; assert(recv);

    IP_segment seg = *(IP_segment*) recvd_segment;
    free(recvd_segment);

    err = ip_assemble(&seg);
    switch (err_no(err))
    {
    case NET_THROW_IPv4_SEG:
    {
        EVENT_INFO("A Segmented IP message received, Can't free the buffer now");
        break;
    }
    case NET_ERR_IPv4_DUPLITCATE_SEG:
    {
        EVENT_INFO("A duplicated IP message received, free the buffer now");
        free_buffer(seg.buf);
        break;
    }
    case SYS_ERR_OK:
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}

void event_ipv4_handle(void* recv) {
    errval_t err; assert(recv);

    IP_handle handle = *(IP_handle*) recv;
    free(recv);

    err = ipv4_handle(handle.ip, handle.proto, handle.src_ip, handle.buf);
    switch (err_no(err))
    {
    case NET_THROW_SUBMIT_EVENT:
    {
        EVENT_INFO("An Event is submitted, and the buffer is re-used, can't free now");
        break;
    }
    case SYS_ERR_OK:
        free_buffer(handle.buf); 
        break;
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}

void event_ndp_marshal(void* send) {
    errval_t err; assert(send);

    NDP_marshal marshal = *(NDP_marshal*) send;
    free(send);

    err = ndp_marshal(marshal.icmp, marshal.dst_ip, marshal.type, marshal.code, marshal.buf);
    switch (err_no(err))
    {
    case NET_THROW_SUBMIT_EVENT:
        break;
    case SYS_ERR_OK:
        free_buffer(marshal.buf);
        break;
    default:
        USER_PANIC_ERR(err, "Unknown error");
    }
}