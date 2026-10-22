/**
 * @file cmd_reboot.c
 * @brief reboot: guarded software restart with a bounded acknowledgement
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
 * REQ-FUNC-29..32, REQ-FUNC-62; tech_spec.md 2.6.6 and 2.4.4. The guard
 * (ble_ota_reboot_guard_commit) decides atomically with respect to OTA RX on
 * the host task. ESP_OK from it means COMMITTED, and from that point there is
 * no code path that returns to the dispatcher: this function restarts
 * unconditionally (INV-R1). Every step is bounded by ONE deadline,
 * t_cmd + 1410 ms, so esp_restart() is called within 1440 ms of the command
 * for every Kconfig value in range and every guard outcome. Nothing here
 * touches NVS (bonds, Wi-Fi configuration and the hardware revision persist).
 */
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_ota.h"
#include "diag_format.h"
#include "diag_internal.h"

static const char *TAG = DIAG_TAG;

#define REBOOT_WRITE_FLUSH_BUDGET_US 1410000LL
#define REBOOT_LOG_UART_WAIT_MS      20u

static void refuse(const diag_ctx_t *ctx, esp_err_t rc)
{
    switch (rc) {
    case ESP_ERR_BLE_OTA_REBOOT_STALE:
        /* The originating BLE session is gone: nobody to answer. */
        ESP_LOGW(TAG, "REBOOT_REJECT reason=stale_session");
        break;
    case ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE:
        (void)diag_emit_error(ctx, "busy", "reboot", "reason=ota_active");
        ESP_LOGW(TAG, "REBOOT_REJECT reason=ota_active");
        break;
    case ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY:
        (void)diag_emit_error(ctx, "busy", "reboot", "reason=pending_verify");
        ESP_LOGW(TAG, "REBOOT_REJECT reason=pending_verify");
        break;
    case ESP_ERR_TIMEOUT:
        /* Covers exec TIMEOUT and INDETERMINATE after the guard's closing CAS won:
         * nothing was or will be committed. */
        (void)diag_emit_error(ctx, "internal", "reboot", NULL);
        ESP_LOGW(TAG, "REBOOT_REJECT reason=host_timeout");
        break;
    default:
        (void)diag_emit_error(ctx, "internal", "reboot", NULL);
        ESP_LOGW(TAG, "REBOOT_REJECT reason=internal");
        break;
    }
}

esp_err_t diag_cmd_reboot(const diag_ctx_t *ctx)
{
    const int64_t t_cmd = esp_timer_get_time();

    if (ctx->transport == DIAG_TRANSPORT_BLE && ctx->gen == 0u) {
        refuse(ctx, ESP_ERR_INVALID_STATE); /* a BLE request must carry its generation */
        return ESP_FAIL;
    }
    const uint32_t ble_gen = (ctx->transport == DIAG_TRANSPORT_BLE) ? ctx->gen : 0u;

    bool override = false;
    esp_err_t rc = ble_ota_reboot_guard_commit(ble_gen, CONFIG_DIAG_REBOOT_GUARD_TIMEOUT_MS, &override);
    if (rc != ESP_OK) {
        refuse(ctx, rc);
        /* "slot already open" is shown to the user as ERR internal, so it must not be
         * logged as result=busy (the dispatcher maps ESP_ERR_INVALID_STATE to busy). */
        return (rc == ESP_ERR_INVALID_STATE) ? ESP_FAIL : rc;
    }

    /* COMMITTED: from here the device restarts, whatever happens to the link. */
    diag_session_cancel(DIAG_TRANSPORT_ANY, 0, DIAG_CANCEL_REBOOT);

    diag_ctx_t c = *ctx; /* local copy with the single shared deadline */
    c.deadline_us = t_cmd + REBOOT_WRITE_FLUSH_BUDGET_US;
    char line[DIAG_LINE_MAX];
    if (diag_fmt_ok_text(line, sizeof(line), "rebooting") == ESP_OK) {
        (void)diag_write_line(&c, line);
    }

    int64_t rem_us = c.deadline_us - esp_timer_get_time();
    uint32_t ack_ms = (rem_us > 0) ? (uint32_t)(rem_us / 1000) : 0u;
    if (ack_ms > CONFIG_DIAG_REBOOT_ACK_WAIT_MS) {
        ack_ms = CONFIG_DIAG_REBOOT_ACK_WAIT_MS;
    }
    bool delivered = (c.flush != NULL) && (c.flush(&c, ack_ms) == ESP_OK);

    ESP_LOGW(TAG, "REBOOT_ACK src=%s delivered=%d wait_ms=%" PRIu32 "%s", diag_transport_name(ctx->transport),
             delivered ? 1 : 0, ack_ms, override ? " override=drain_stuck" : "");
    vTaskDelay(pdMS_TO_TICKS(REBOOT_LOG_UART_WAIT_MS));
    esp_restart();
}
