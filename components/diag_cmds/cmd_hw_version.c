/**
 * @file cmd_hw_version.c
 * @brief hw_version: report the hardware revision stored in NVS
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
 * REQ-FUNC-20..22. OK hw_version=<rev> / ERR unavailable cmd=hw_version
 * (absent, invalid content or NVS unavailable). Nothing is compiled in.
 */
#include "device_info.h"
#include "diag_format.h"
#include "diag_internal.h"

esp_err_t diag_cmd_hw_version(const diag_ctx_t *ctx)
{
    char rev[DEVICE_INFO_HW_REV_MAX_LEN + 1];
    char line[DIAG_LINE_MAX];

    esp_err_t err = device_info_get_hw_revision(rev, sizeof(rev));
    if (err == ESP_OK && diag_fmt_ok_kv(line, sizeof(line), "hw_version", rev) == ESP_OK) {
        (void)diag_write_line(ctx, line);
        return ESP_OK;
    }
    (void)diag_emit_error(ctx, "unavailable", "hw_version", NULL);
    return ESP_ERR_NOT_FOUND; /* logged as result=unavailable */
}
