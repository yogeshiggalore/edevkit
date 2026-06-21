/*
 * usb_dap_class.c — custom TinyUSB class driver for CMSIS-DAP v2.
 *
 * Two ring buffers (request / response) hold whole DAP packets. The
 * USB OUT ISR appends to req_ring; the main loop drains req_ring,
 * runs the dispatcher, fills resp_ring; the USB IN ISR consumes
 * resp_ring on transfer completion.
 *
 * SPSC discipline (each ring has exactly one producer and one
 * consumer) plus 32-bit monotonic indices means we don't need locks —
 * each side reads the other's pointer at most once per slot and never
 * writes it. The wptr/rptr ints are volatile so the compiler doesn't
 * cache them across loop iterations.
 *
 * The class driver hooks into TinyUSB via usbd_app_driver_get_cb(),
 * which TinyUSB calls at init time to discover any application-provided
 * class drivers. We register exactly one — the DAP one — and return.
 */

#include <string.h>

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "usb_dap_class.h"
#include "usb_descriptors.h"
#include "dap/dap.h"

#define IDX(x) ((x) % EDEV_DAP_PACKET_COUNT)

typedef struct {
    uint8_t  data    [EDEV_DAP_PACKET_COUNT][EDEV_DAP_PACKET_SIZE];
    uint16_t data_len[EDEV_DAP_PACKET_COUNT];
    volatile uint32_t wptr;
    volatile uint32_t rptr;
} ring_t;

static ring_t req_ring;
static ring_t resp_ring;

static uint8_t s_rhport;
static uint8_t s_itf_num;
static uint8_t s_out_ep;
static uint8_t s_in_ep;
static uint8_t s_swo_ep;

static bool s_in_busy;
static bool s_swo_busy;
static bool s_req_repost_pending; /* OUT ISR couldn't repost; main loop will */

static inline bool ring_full (const ring_t *r) { return (uint32_t)(r->wptr - r->rptr) >= EDEV_DAP_PACKET_COUNT; }
static inline bool ring_empty(const ring_t *r) { return r->wptr == r->rptr; }

/* ---------------------------------------------------------------------
 * TinyUSB class-driver callbacks
 * ------------------------------------------------------------------ */

static void dap_init_cb(void)
{
    memset(&req_ring,  0, sizeof(req_ring));
    memset(&resp_ring, 0, sizeof(resp_ring));
    s_in_busy = false;
    s_swo_busy = false;
    s_req_repost_pending = false;
}

static bool dap_deinit_cb(void)
{
    return true;
}

static void dap_reset_cb(uint8_t rhport)
{
    (void)rhport;
    req_ring.wptr  = req_ring.rptr  = 0;
    resp_ring.wptr = resp_ring.rptr = 0;
    s_in_busy = false;
    s_swo_busy = false;
    s_req_repost_pending = false;
    s_itf_num = 0;
}

static uint16_t dap_open_cb(uint8_t                      rhport,
                            tusb_desc_interface_t const *itf_desc,
                            uint16_t                     max_len)
{
    /* Confirm this is our interface — vendor class, no specific sub or
     * protocol. TinyUSB tries every class driver in turn until one
     * accepts the interface, so a quick reject keeps the dispatch
     * tidy. */
    TU_VERIFY(itf_desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC, 0);
    TU_VERIFY(itf_desc->bInterfaceSubClass == 0x00,                    0);
    TU_VERIFY(itf_desc->bInterfaceProtocol == 0x00,                    0);

    uint16_t drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                                  (size_t)itf_desc->bNumEndpoints *
                                  sizeof(tusb_desc_endpoint_t));
    TU_VERIFY(max_len >= drv_len, 0);

    s_rhport  = rhport;
    s_itf_num = itf_desc->bInterfaceNumber;

    /* Walk the endpoints in descriptor order. usb_descriptors.c emits
     * them as: DAP OUT, DAP IN, SWO IN — don't reorder there without
     * also updating this loop. */
    tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)(itf_desc + 1);

    s_out_ep = ep->bEndpointAddress;
    usbd_edpt_open(rhport, ep);
    /* Prime the OUT endpoint so the host's first DAP packet has a buffer
     * waiting. We arm at the current write slot — wptr is 0 at this
     * point so this targets slot 0. */
    usbd_edpt_xfer(rhport, s_out_ep, req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);

    ep++;
    s_in_ep = ep->bEndpointAddress;
    usbd_edpt_open(rhport, ep);

    if (itf_desc->bNumEndpoints >= 3) {
        ep++;
        s_swo_ep = ep->bEndpointAddress;
        usbd_edpt_open(rhport, ep);
    } else {
        s_swo_ep = 0;
    }

    return drv_len;
}

static bool dap_control_xfer_cb(uint8_t                       rhport,
                                uint8_t                       stage,
                                tusb_control_request_t const *req)
{
    /* MS OS 2.0 BOS fetch is a DEVICE-recipient vendor request and is
     * handled in usb_descriptors.c::tud_vendor_control_xfer_cb. No
     * class-level control requests in v0.1. */
    (void)rhport; (void)stage; (void)req;
    return false;
}

static bool dap_xfer_cb(uint8_t       rhport,
                        uint8_t       ep_addr,
                        xfer_result_t result,
                        uint32_t      xferred)
{
    /* Transfer failure path — host yanked the cable, USB suspended, or
     * we hit a stall. Re-prime the OUT endpoint with the SAME slot
     * (don't advance wptr because we never got a complete packet) and
     * for IN/SWO just clear busy so the main loop will retry. */
    if (result != XFER_RESULT_SUCCESS) {
        if (ep_addr == s_out_ep) {
            usbd_edpt_xfer(rhport, s_out_ep,
                           req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);
        } else if (ep_addr == s_in_ep) {
            s_in_busy = false;
        } else if (ep_addr == s_swo_ep) {
            s_swo_busy = false;
        }
        return true;
    }

    if (ep_addr == s_out_ep) {
        if (xferred > EDEV_DAP_PACKET_SIZE) {
            /* This shouldn't happen — the OUT buffer is sized at
             * EDEV_DAP_PACKET_SIZE — but be defensive. */
            return false;
        }
        if (xferred == 0) {
            /* USB allows zero-length packets (ZLPs) as end-of-transfer
             * markers; they're not a DAP command. Re-prime the same
             * slot rather than tag it with len=0 and confuse the
             * dispatcher. */
            usbd_edpt_xfer(rhport, s_out_ep,
                           req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);
            return true;
        }

        req_ring.data_len[IDX(req_ring.wptr)] = (uint16_t)xferred;
        req_ring.wptr++;

        if (ring_full(&req_ring)) {
            /* No room — defer the next OUT prime until the main loop
             * drains a slot. */
            s_req_repost_pending = true;
        } else {
            usbd_edpt_xfer(rhport, s_out_ep,
                           req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);
        }
        return true;
    }

    if (ep_addr == s_in_ep) {
        /* Previous IN xfer reached the host. Free its slot; if the
         * response ring still has data, kick the next one. */
        resp_ring.rptr++;
        s_in_busy = false;

        if (!ring_empty(&resp_ring)) {
            uint16_t len = resp_ring.data_len[IDX(resp_ring.rptr)];
            if (usbd_edpt_xfer(rhport, s_in_ep,
                               resp_ring.data[IDX(resp_ring.rptr)], len)) {
                s_in_busy = true;
            }
        }
        return true;
    }

    if (ep_addr == s_swo_ep) {
        s_swo_busy = false;
        return true;
    }

    return false;
}

static const usbd_class_driver_t s_dap_driver = {
    .init             = dap_init_cb,
    .deinit           = dap_deinit_cb,
    .reset            = dap_reset_cb,
    .open             = dap_open_cb,
    .control_xfer_cb  = dap_control_xfer_cb,
    .xfer_cb          = dap_xfer_cb,
    .sof              = NULL,
#if CFG_TUSB_DEBUG >= 2
    .name             = "edev_dapv2 CMSIS-DAP v2",
#endif
};

/* TinyUSB calls this to discover application class drivers. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_dap_driver;
}

/* ---------------------------------------------------------------------
 * SWO endpoint surface
 * ------------------------------------------------------------------ */

bool edev_dap_class_swo_submit(const uint8_t *buf, uint16_t len)
{
    if (s_swo_ep == 0 || s_swo_busy || len == 0) {
        return false;
    }
    if (!usbd_edpt_claim(s_rhport, s_swo_ep)) {
        return false;
    }
    if (!usbd_edpt_xfer(s_rhport, s_swo_ep, (uint8_t *)buf, len)) {
        usbd_edpt_release(s_rhport, s_swo_ep);
        return false;
    }
    s_swo_busy = true;
    return true;
}

bool edev_dap_class_swo_is_busy(void)
{
    return s_swo_busy;
}

/* ---------------------------------------------------------------------
 * Main-loop pump
 * ------------------------------------------------------------------ */

void edev_dap_class_task(void)
{
    /* If the OUT callback couldn't repost because the request ring was
     * full, try here as soon as a slot frees up. */
    if (s_req_repost_pending && !ring_full(&req_ring)) {
        s_req_repost_pending = false;
        usbd_edpt_xfer(s_rhport, s_out_ep,
                       req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);
    }

    /* Drain request slots into the dispatcher. We stop when either
     * the request ring is empty or the response ring is full. */
    while (!ring_empty(&req_ring) && !ring_full(&resp_ring)) {
        uint32_t ri = IDX(req_ring.rptr);
        uint32_t wi = IDX(resp_ring.wptr);

        /* DAP_QueueCommands (0x7E) — hosts (notably pyOCD) sometimes
         * send this, but it has the same semantics as ExecuteCommands
         * for our purposes (run the bundle, return responses), and
         * QueueCommands is the more complex one because it expects
         * deferred execution across packets. Rewrite to 0x7F.
         * Matches the workaround in raspberrypi/debugprobe. */
        if (req_ring.data[ri][0] == 0x7E) {
            req_ring.data[ri][0] = 0x7F;
        }

        uint16_t resp_len = dap_dispatch(req_ring.data[ri],
                                         req_ring.data_len[ri],
                                         resp_ring.data[wi],
                                         EDEV_DAP_PACKET_SIZE);

        resp_ring.data_len[wi] = resp_len;
        resp_ring.wptr++;
        req_ring.rptr++;

        if (s_req_repost_pending) {
            s_req_repost_pending = false;
            usbd_edpt_xfer(s_rhport, s_out_ep,
                           req_ring.data[IDX(req_ring.wptr)], EDEV_DAP_PACKET_SIZE);
        }
    }

    /* Kick the IN endpoint if it's idle and we have a response queued. */
    if (!s_in_busy && !ring_empty(&resp_ring)) {
        uint16_t len = resp_ring.data_len[IDX(resp_ring.rptr)];
        if (usbd_edpt_claim(s_rhport, s_in_ep)) {
            if (usbd_edpt_xfer(s_rhport, s_in_ep,
                               resp_ring.data[IDX(resp_ring.rptr)], len)) {
                s_in_busy = true;
            } else {
                usbd_edpt_release(s_rhport, s_in_ep);
            }
        }
    }
}
