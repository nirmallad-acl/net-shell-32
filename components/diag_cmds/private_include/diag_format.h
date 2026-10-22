/**
 * @file diag_format.h
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
 */

/*
 * Grammar (REQ-FUNC-10): OK <...> | DATA <...> | ERR <code>[ cmd=<name>][ key=value...].
 * Every function writes one line without LF into buf (cap should be
 * DIAG_LINE_MAX = 128) and returns ESP_OK, ESP_ERR_INVALID_ARG (NULL, bad code
 * or non-printable text) or ESP_ERR_INVALID_SIZE (line would exceed 127
 * characters or cap). On any error buf[0] is set to '\0'. No float printf is used.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_FMT_LINE_MAX_CHARS 127u

bool      diag_err_code_valid(const char *code);

esp_err_t diag_fmt_ok_kv(char *buf, size_t cap, const char *key, const char *value);
esp_err_t diag_fmt_ok_text(char *buf, size_t cap, const char *text);
esp_err_t diag_fmt_err(char *buf, size_t cap, const char *code, const char *cmd, const char *extra);
esp_err_t diag_fmt_reboot_reason(char *buf, size_t cap, const char *token, int code);
esp_err_t diag_fmt_accel_start(char *buf, size_t cap);
esp_err_t diag_fmt_accel_data(char *buf, size_t cap, uint32_t n, int32_t x_mg, int32_t y_mg, int32_t z_mg,
                              uint32_t age_ms, bool hardware);
esp_err_t diag_fmt_accel_err(char *buf, size_t cap, const char *code, uint32_t n);
esp_err_t diag_fmt_accel_complete(char *buf, size_t cap, uint32_t samples, uint32_t errors);

/* [-]I.FFF (3 decimals, sign only for negatives); values are clamped to +/-99999 mg. */
esp_err_t diag_fmt_mg(char *buf, size_t cap, int32_t mg);
/* lroundf(g * 1000), clamped to +/-99999. */
int32_t   diag_g_to_mg(float g);

#ifdef __cplusplus
}
#endif
