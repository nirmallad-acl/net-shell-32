/**
 * @file cmd_accel_xyz.c
 * @brief accel_xyz: start the system-wide 15-sample, 1 s interval acceleration session
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-10-19
 *
 * @copyright Copyright (c) 2026 ACL Digital Pvt Ltd. All rights reserved.
 *
 * CONFIDENTIALITY NOTICE:
 * This software and documentation are the confidential and proprietary
 * information of ACL Digital Pvt Ltd. Unauthorized copying, distribution,
 * modification, or reverse engineering of this file, via any medium,
 * is strictly prohibited.
 *
 * REQ-FUNC-40..49. BLE: the worker returns at once and the session task emits
 * the samples. Serial: the REPL handler waits for the completion line (15 s +
 * 2 s) so the prompt returns after it (OQ-18); on timeout it cancels and
 * answers ERR internal.
 */
#include "accel_session.h"
#include "diag_internal.h"

#define SERIAL_WAIT_MS (15000u + 2000u)

esp_err_t diag_cmd_accel_xyz(const diag_ctx_t *ctx)
{
    esp_err_t err = accel_session_start(ctx);
    if (err == ESP_ERR_INVALID_STATE) {
        (void)diag_emit_error(ctx, "busy", "accel_xyz", NULL);
        return err;
    }
    if (err == ESP_FAIL) {
        return err; /* the start line could not be sent: the peer is gone, nothing to say */
    }
    if (err != ESP_OK) {
        (void)diag_emit_error(ctx, "internal", "accel_xyz", NULL);
        return err;
    }
    if (ctx->transport == DIAG_TRANSPORT_SERIAL) {
        if (!accel_session_wait_done(SERIAL_WAIT_MS)) {
            diag_session_cancel(DIAG_TRANSPORT_SERIAL, 0, DIAG_CANCEL_TIMEOUT);
            (void)diag_emit_error(ctx, "internal", "accel_xyz", NULL);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}
