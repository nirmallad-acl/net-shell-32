/**
 * @file diag_format.c
 * @brief PURE: every response line, milli-g formatting and the response grammar
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
 * REQ-FUNC-10, 40, 43, 45; tech_spec.md 2.6.3 and 2.6.5.
 */
#include "diag_format.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MG_LIMIT 99999

static const char *const s_codes[] = {
    "unknown_command", "invalid_argument", "oversized_input", "unavailable",
    "sensor_error", "sensor_stale", "busy", "internal",
};

bool diag_err_code_valid(const char *code)
{
    if (code == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_codes) / sizeof(s_codes[0]); i++) {
        if (strcmp(code, s_codes[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Completes a snprintf-built line: size check and printable-ASCII check. */
static esp_err_t finish(char *buf, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap || (size_t)n > DIAG_FMT_LINE_MAX_CHARS) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20u || c > 0x7Eu) {
            buf[0] = '\0';
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static bool args_ok(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return false;
    }
    buf[0] = '\0';
    return true;
}

esp_err_t diag_fmt_ok_kv(char *buf, size_t cap, const char *key, const char *value)
{
    if (!args_ok(buf, cap) || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return finish(buf, cap, snprintf(buf, cap, "OK %s=%s", key, value));
}

esp_err_t diag_fmt_ok_text(char *buf, size_t cap, const char *text)
{
    if (!args_ok(buf, cap) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return finish(buf, cap, snprintf(buf, cap, "OK %s", text));
}

esp_err_t diag_fmt_err(char *buf, size_t cap, const char *code, const char *cmd, const char *extra)
{
    if (!args_ok(buf, cap) || !diag_err_code_valid(code)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(buf, cap, "ERR %s", code);
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    if (cmd != NULL && cmd[0] != '\0') {
        int m = snprintf(buf + n, cap - (size_t)n, " cmd=%s", cmd);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            buf[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        n += m;
    }
    if (extra != NULL && extra[0] != '\0') {
        int m = snprintf(buf + n, cap - (size_t)n, " %s", extra);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            buf[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        n += m;
    }
    return finish(buf, cap, n);
}

esp_err_t diag_fmt_reboot_reason(char *buf, size_t cap, const char *token, int code)
{
    if (!args_ok(buf, cap) || token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return finish(buf, cap, snprintf(buf, cap, "OK reboot_reason=%s code=%d", token, code));
}

esp_err_t diag_fmt_accel_start(char *buf, size_t cap)
{
    if (!args_ok(buf, cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    return finish(buf, cap, snprintf(buf, cap, "OK accel_xyz started interval_ms=1000 samples=15"));
}

int32_t diag_g_to_mg(float g)
{
    float v = g * 1000.0f;
    if (!(v == v)) { /* NaN */
        return 0;
    }
    if (v > (float)MG_LIMIT) {
        return MG_LIMIT;
    }
    if (v < -(float)MG_LIMIT) {
        return -MG_LIMIT;
    }
    return (int32_t)lroundf(v);
}

esp_err_t diag_fmt_mg(char *buf, size_t cap, int32_t mg)
{
    if (!args_ok(buf, cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mg > MG_LIMIT) {
        mg = MG_LIMIT;
    }
    if (mg < -MG_LIMIT) {
        mg = -MG_LIMIT;
    }
    uint32_t a = (mg < 0) ? (uint32_t)(-mg) : (uint32_t)mg;
    return finish(buf, cap, snprintf(buf, cap, "%s%" PRIu32 ".%03" PRIu32, (mg < 0) ? "-" : "",
                                     a / 1000u, a % 1000u));
}

esp_err_t diag_fmt_accel_data(char *buf, size_t cap, uint32_t n, int32_t x_mg, int32_t y_mg, int32_t z_mg,
                              uint32_t age_ms, bool hardware)
{
    if (!args_ok(buf, cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    char xs[16];
    char ys[16];
    char zs[16];
    if (diag_fmt_mg(xs, sizeof(xs), x_mg) != ESP_OK || diag_fmt_mg(ys, sizeof(ys), y_mg) != ESP_OK ||
        diag_fmt_mg(zs, sizeof(zs), z_mg) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    return finish(buf, cap,
                  snprintf(buf, cap, "DATA sample=%" PRIu32 " x=%s y=%s z=%s unit=g age_ms=%" PRIu32 " source=%s",
                           n, xs, ys, zs, age_ms, hardware ? "hardware" : "simulated"));
}

esp_err_t diag_fmt_accel_err(char *buf, size_t cap, const char *code, uint32_t n)
{
    if (!args_ok(buf, cap) || !diag_err_code_valid(code)) {
        return ESP_ERR_INVALID_ARG;
    }
    return finish(buf, cap, snprintf(buf, cap, "ERR %s cmd=accel_xyz sample=%" PRIu32, code, n));
}

esp_err_t diag_fmt_accel_complete(char *buf, size_t cap, uint32_t samples, uint32_t errors)
{
    if (!args_ok(buf, cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (errors == 0u) {
        return finish(buf, cap, snprintf(buf, cap, "OK accel_xyz complete samples=%" PRIu32, samples));
    }
    return finish(buf, cap,
                  snprintf(buf, cap, "OK accel_xyz complete samples=%" PRIu32 " errors=%" PRIu32, samples, errors));
}
