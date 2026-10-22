/**
 * @file cmd_fw_version.c
 * @brief fw_version: report the running image's version from the app descriptor
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
 * REQ-FUNC-23..26. OK fw_version=<ver> / ERR unavailable cmd=fw_version
 * (empty, placeholder "1" or invalid version string).
 */
#include "device_info.h"
#include "diag_format.h"
#include "diag_internal.h"

esp_err_t diag_cmd_fw_version(const diag_ctx_t *ctx)
{
    char ver[DEVICE_INFO_FW_VER_MAX_LEN + 1];
    char line[DIAG_LINE_MAX];

    esp_err_t err = device_info_get_fw_version(ver, sizeof(ver));
    if (err == ESP_OK && diag_fmt_ok_kv(line, sizeof(line), "fw_version", ver) == ESP_OK) {
        (void)diag_write_line(ctx, line);
        return ESP_OK;
    }
    (void)diag_emit_error(ctx, "unavailable", "fw_version", NULL);
    return ESP_ERR_NOT_FOUND; /* logged as result=unavailable */
}
