/*
 * dap_swo.c — SWO group stubs.
 *
 * SWO is NOT advertised in the capability byte for v0.1 — host tools
 * shouldn't invoke any of these. If they do, we return DAP_ERROR or
 * a benign zero-value response.
 */

#include "dap/dap_internal.h"

uint16_t dap_handle_swo_xport(const uint8_t *req, uint16_t req_len,
                              uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

uint16_t dap_handle_swo_mode(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

uint16_t dap_handle_swo_baud(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 4) return 0;
    /* baudrate echo = 0 → "not supported". */
    resp[0] = 0; resp[1] = 0; resp[2] = 0; resp[3] = 0;
    return 4;
}

uint16_t dap_handle_swo_ctrl(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

uint16_t dap_handle_swo_status(const uint8_t *req, uint16_t req_len,
                               uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 5) return 0;
    /* status=0, count=0 */
    resp[0] = 0;
    resp[1] = 0; resp[2] = 0; resp[3] = 0; resp[4] = 0;
    return 5;
}

uint16_t dap_handle_swo_data(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 3) return 0;
    /* trace status=0, count=0 */
    resp[0] = 0;
    resp[1] = 0; resp[2] = 0;
    return 3;
}

uint16_t dap_handle_swo_estat(const uint8_t *req, uint16_t req_len,
                              uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 13) return 0;
    /* status=0, count=0, index=0, td_timestamp=0 */
    for (int i = 0; i < 13; ++i) resp[i] = 0;
    return 13;
}
