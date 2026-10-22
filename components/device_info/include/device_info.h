/**
 * @file device_info.h
 * @brief Public interface of device_info: firmware version, hardware revision, reset reason
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
 * NETSHELL32-5, tech_spec.md sections 2.2.5 and 2.8. Reusable without any
 * Bluetooth dependency (REQ-FUNC-26). Only <stdbool.h>, <stddef.h> and
 * esp_err.h types appear here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_INFO_HW_REV_MAX_LEN 32 /* characters, excluding NUL */
#define DEVICE_INFO_FW_VER_MAX_LEN 31

/* Captures esp_reset_reason() once. nvs_ok is the boot-time NVS result:
 * false makes the hardware revision getter return ESP_ERR_INVALID_STATE. */
esp_err_t device_info_init(bool nvs_ok);

/* All getters: ESP_ERR_INVALID_ARG (NULL), ESP_ERR_INVALID_SIZE (buffer too
 * small; nothing written beyond buf[0] = '\0'), ESP_ERR_NOT_FOUND (absent or
 * placeholder), ESP_ERR_INVALID_RESPONSE (stored content fails validation),
 * ESP_ERR_INVALID_STATE (NVS unavailable). */
esp_err_t device_info_get_fw_version(char *buf, size_t buf_len);
esp_err_t device_info_get_hw_revision(char *buf, size_t buf_len);

/* Validates first (stored value unchanged on failure), then writes and
 * commits. ESP_ERR_INVALID_ARG on a NULL or invalid revision. */
esp_err_t device_info_set_hw_revision(const char *rev);
bool      device_info_hw_revision_is_valid(const char *rev);

/* token is a static string, never NULL on ESP_OK. Either output may be NULL. */
esp_err_t   device_info_get_reset_reason(const char **token, int *code);
/* Never NULL, never reads out of bounds: unknown for any unmapped code. */
const char *device_info_reset_reason_token(int code);

#ifdef __cplusplus
}
#endif
