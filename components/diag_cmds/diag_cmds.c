/**
 * @file diag_cmds.c
 * @brief Dispatcher of the shared command layer: closed BLE table, BLECLI_CMD log, init
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
 * REQ-FUNC-13 (closed set of five), REQ-FUNC-14/15/17, REQ-TEST-04.
 */
#include "diag_cmds.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "ble_ota.h"

#include "accel_session.h"
#include "diag_format.h"
#include "diag_internal.h"

static const char *TAG = DIAG_TAG;

typedef struct {
    const char *name;
    esp_err_t (*handler)(const diag_ctx_t *ctx);
} diag_cmd_entry_t;

/* The closed set of five commands, indexed by diag_cmd_id_t. hw_rev_set is NOT
 * here: it is serial-only (diag_serial.c) and unreachable from BLE. */
static const diag_cmd_entry_t s_cmds[DIAG_CMD_COUNT] = {
    [DIAG_CMD_HW_VERSION]    = { "hw_version", diag_cmd_hw_version },
    [DIAG_CMD_FW_VERSION]    = { "fw_version", diag_cmd_fw_version },
    [DIAG_CMD_ACCEL_XYZ]     = { "accel_xyz", diag_cmd_accel_xyz },
    [DIAG_CMD_REBOOT_REASON] = { "reboot_reason", diag_cmd_reboot_reason },
    [DIAG_CMD_REBOOT]        = { "reboot", diag_cmd_reboot },
};

uint32_t diag_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *diag_transport_name(diag_transport_t t)
{
    switch (t) {
    case DIAG_TRANSPORT_SERIAL: return "serial";
    case DIAG_TRANSPORT_BLE:    return "ble";
    default:                    return "any";
    }
}

esp_err_t diag_cmds_init(void)
{
    esp_err_t err = accel_session_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "accel session init failed (%s); accel_xyz unavailable", esp_err_to_name(err));
    }
    esp_err_t rerr = diag_resmon_init();
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "resource monitor init failed (%s)", esp_err_to_name(rerr));
    }
    return (err != ESP_OK) ? err : rerr;
}

esp_err_t diag_cmds_register_serial(void)
{
    return diag_serial_register();
}

esp_err_t diag_cmd_lookup_ble(const char *name, size_t len, diag_cmd_id_t *out)
{
    if (name == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < (int)DIAG_CMD_COUNT; i++) {
        if (strlen(s_cmds[i].name) == len && memcmp(s_cmds[i].name, name, len) == 0) {
            *out = (diag_cmd_id_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *diag_cmd_name(diag_cmd_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)DIAG_CMD_COUNT) {
        return "?";
    }
    return s_cmds[id].name;
}

esp_err_t diag_write_line(const diag_ctx_t *ctx, const char *line)
{
    if (ctx == NULL || ctx->write_line == NULL || line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(line);
    if (n == 0 || n > DIAG_FMT_LINE_MAX_CHARS) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ctx->write_line(ctx, line, n);
}

esp_err_t diag_emit_error(const diag_ctx_t *ctx, const char *code, const char *cmd, const char *extra)
{
    char line[DIAG_LINE_MAX];
    esp_err_t err = diag_fmt_err(line, sizeof(line), code, cmd, extra);
    if (err != ESP_OK) {
        /* Never leave the peer without an answer: fall back to the generic error. */
        err = diag_fmt_err(line, sizeof(line), "internal", cmd, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }
    return diag_write_line(ctx, line);
}

/* Result token of the BLECLI_CMD log line. Handlers return ESP_OK on an OK/DATA
 * outcome, ESP_ERR_NOT_FOUND for "unavailable", ESP_ERR_INVALID_STATE for "busy"
 * and the ble_ota guard refusal codes for a refused reboot. */
static const char *result_token(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ok";
    case ESP_ERR_NOT_FOUND:
        return "unavailable";
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE:
    case ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY:
        return "busy";
    case ESP_ERR_INVALID_ARG:
        return "invalid_argument";
    default:
        return "internal";
    }
}

esp_err_t diag_cmd_run(diag_cmd_id_t id, const diag_ctx_t *ctx)
{
    if ((int)id < 0 || (int)id >= (int)DIAG_CMD_COUNT || ctx == NULL || ctx->write_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = s_cmds[id].handler(ctx);
    /* t_ms = completion time; t_rx_ms = when the full line reached the CLI layer
     * (PERF-01: device latency = t_ms - t_rx_ms). Fields only ever appended. */
    ESP_LOGI(TAG, "BLECLI_CMD src=%s name=%s result=%s t_ms=%" PRIu32 " t_rx_ms=%" PRIu32,
             diag_transport_name(ctx->transport), s_cmds[id].name, result_token(err), diag_now_ms(),
             ctx->t_rx_ms);
    return err;
}
