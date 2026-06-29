/*
 * usb_dap_class.c — custom TinyUSB class driver for the DAP interface.
 *
 * Why a custom driver and not stock CFG_TUD_VENDOR? CMSIS-DAP is a
 * strict request/response protocol where "one USB OUT transfer = one
 * DAP packet" must hold. TinyUSB's stock vendor class uses internal
 * RX/TX FIFOs that hide packet boundaries — fine for streamed data,
 * disastrous for DAP.
 *
 * This driver:
 *   1. Owns interface 0 (the DAP vendor interface).
 *   2. Claims EP 0x01 OUT and EP 0x81 IN.
 *   3. Posts a 512-byte receive on the OUT EP; on completion, hands
 *      the buffer to dap_dispatch() which fills a response.
 *   4. Posts the response on the IN EP.
 *   5. Repeats — strict ping-pong, one in flight at a time.
 *
 * It also handles the MS OS 2.0 descriptor-set vendor request
 * (bRequest = MS_OS_20_VENDOR_REQ_CODE, wIndex = 7) so Windows binds
 * WinUSB without an .inf install.
 */

#include "usb/usb_dap_class.h"
#include "usb/usb_descriptors.h"

#include "dap/dap.h"
#include "dap/dap_config.h"

#include "tusb.h"
#include "device/usbd_pvt.h"

#include <stdbool.h>
#include <string.h>

/* The MS OS 2.0 vendor request code we advertise in the BOS chain. */
#define MS_OS_20_VENDOR_REQ_CODE     0x01u

/* External MS OS 2.0 descriptor set blob (defined in usb_descriptors.c). */
extern const uint8_t  edev_ms_os_20_desc[];
extern const uint16_t edev_ms_os_20_desc_len;

/* Single in-flight packet — DAP is half-duplex by design. */
static CFG_TUSB_MEM_ALIGN uint8_t s_req_buf [DAP_PACKET_SIZE];
static CFG_TUSB_MEM_ALIGN uint8_t s_resp_buf[DAP_PACKET_SIZE];

static volatile uint16_t s_req_len;        /* request bytes received */
static volatile bool     s_req_ready;      /* request waiting for dispatch */
static volatile bool     s_resp_in_flight; /* response currently being sent */

static uint8_t s_rhport;

/* ----------------------------------------------------------------------
 * TinyUSB custom class driver callbacks
 * ---------------------------------------------------------------------- */

static void dap_class_init(void)
{
    s_req_len = 0;
    s_req_ready = false;
    s_resp_in_flight = false;
}

static bool dap_class_deinit(void)
{
    return true;
}

static void dap_class_reset(uint8_t rhport)
{
    (void) rhport;
    s_req_len = 0;
    s_req_ready = false;
    s_resp_in_flight = false;
}

static uint16_t dap_class_open(uint8_t rhport,
                               tusb_desc_interface_t const *itf_desc,
                               uint16_t max_len)
{
    /* Match only our specific interface. */
    if (itf_desc->bInterfaceNumber != EDEV_DAP_ITF_NUM
        || itf_desc->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) {
        return 0;
    }

    /* Skip past the interface descriptor and open the two bulk EPs. */
    uint8_t const *p = (uint8_t const *) itf_desc + itf_desc->bLength;
    uint16_t consumed = itf_desc->bLength;
    int eps_opened = 0;
    while (consumed < max_len && eps_opened < 2) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *) p;
        if (ep->bDescriptorType == TUSB_DESC_ENDPOINT) {
            if (!usbd_edpt_open(rhport, ep)) return 0;
            ++eps_opened;
        }
        consumed += ep->bLength;
        p += ep->bLength;
    }
    if (eps_opened != 2) return 0;

    s_rhport = rhport;

    /* Arm the OUT EP for the first DAP packet. */
    usbd_edpt_xfer(rhport, EDEV_DAP_EP_OUT, s_req_buf, sizeof(s_req_buf));

    return consumed;
}

static bool dap_class_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                      tusb_control_request_t const *request)
{
    /* Only handle SETUP — TinyUSB completes data/status for us. */
    if (stage != CONTROL_STAGE_SETUP) return true;

    /* MS OS 2.0 descriptor set:
     *   bmRequestType = 0xC0 (Device-to-Host, Vendor, Device)
     *   bRequest      = MS_OS_20_VENDOR_REQ_CODE
     *   wIndex        = 7 (MS_OS_20_DESCRIPTOR_INDEX)
     */
    if (request->bmRequestType == 0xC0
        && request->bRequest == MS_OS_20_VENDOR_REQ_CODE
        && request->wIndex == 7) {
        uint16_t len = edev_ms_os_20_desc_len;
        if (request->wLength < len) len = request->wLength;
        return tud_control_xfer(rhport, request,
                                (void *) edev_ms_os_20_desc, len);
    }

    return false;   /* not for us — let other drivers/stack respond */
}

static bool dap_class_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                              xfer_result_t result, uint32_t xferred)
{
    (void) rhport;
    (void) result;

    if (ep_addr == EDEV_DAP_EP_OUT) {
        /* A DAP packet arrived. Stash and let main loop dispatch. */
        s_req_len = (uint16_t) xferred;
        s_req_ready = true;
        return true;
    }

    if (ep_addr == EDEV_DAP_EP_IN) {
        /* Response sent — arm the OUT EP for the next request. */
        s_resp_in_flight = false;
        usbd_edpt_xfer(s_rhport, EDEV_DAP_EP_OUT,
                       s_req_buf, sizeof(s_req_buf));
        return true;
    }

    return false;
}

/* ----------------------------------------------------------------------
 * Registration — TinyUSB calls this during init.
 * ---------------------------------------------------------------------- */

static const usbd_class_driver_t s_dap_class_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name             = "EDEV_DAP",
#endif
    .init             = dap_class_init,
    .deinit           = dap_class_deinit,
    .reset            = dap_class_reset,
    .open             = dap_class_open,
    .control_xfer_cb  = dap_class_control_xfer_cb,
    .xfer_cb          = dap_class_xfer_cb,
    .sof              = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_dap_class_driver;
}

/* ----------------------------------------------------------------------
 * Public API — called from main loop.
 * ---------------------------------------------------------------------- */

void edev_dap_class_init(void)
{
    /* Nothing — TinyUSB calls dap_class_init() via the driver table. */
}

void edev_dap_class_task(void)
{
    /* Anything to dispatch? */
    if (!s_req_ready || s_resp_in_flight) return;

    uint16_t resp_len = dap_dispatch(s_req_buf, s_req_len,
                                     s_resp_buf, sizeof(s_resp_buf));
    s_req_ready = false;

    if (resp_len == 0) {
        /* No response → re-arm OUT EP without sending anything.
         * In practice every DAP command produces a response, so this
         * branch is defensive. */
        usbd_edpt_xfer(s_rhport, EDEV_DAP_EP_OUT,
                       s_req_buf, sizeof(s_req_buf));
        return;
    }

    s_resp_in_flight = true;
    usbd_edpt_xfer(s_rhport, EDEV_DAP_EP_IN, s_resp_buf, resp_len);
}
