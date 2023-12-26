#include <common.h>
#include <netstack/ethernet.h>
#include <event/event.h>
#include <device/device.h>      //DEVICE_HEADER_RESERVE

#include <netutil/dump.h>

void frame_unmarshal(void* frame) {
    errval_t err;
    assert(frame);

    Frame* fr = frame;
    Ethernet *ether = fr->ether;
    uint8_t  *data  = fr->data;
    size_t    size  = fr->size;

    err = ethernet_unmarshal(ether, data, size);
    if (err_no(err) == NET_ERR_IPv4_SEG_LATER_FREE) {
        // We need to keep the buffer for later assembling
        // DONT free it here
    } else if (err_is_fail(err)) {
        DEBUG_ERR(err, "We meet an error when processing this frame, but the process continue");
        free(data - DEVICE_HEADER_RESERVE);
    } else {
        // Handled a frame successfully
        free(data - DEVICE_HEADER_RESERVE);
    }
    
    free(frame);
}