/**
 * @file cmd_reboot_reason.c
 * @brief reboot_reason: report the reset reason captured once at boot
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
 * REQ-FUNC-27, REQ-FUNC-28. OK reboot_reason=<token> code=<n>.
 */
#include "device_info.h"
#include "diag_format.h"
#include "diag_internal.h"

esp_err_t diag_cmd_reboot_reason(const diag_ctx_t *ctx)
{
    const char *tok = NULL;
    int code = 0;
    char line[DIAG_LINE_MAX];

    esp_err_t err = device_info_get_reset_reason(&tok, &code);
    if (err == ESP_OK && tok != NULL && diag_fmt_reboot_reason(line, sizeof(line), tok, code) == ESP_OK) {
        (void)diag_write_line(ctx, line);
        return ESP_OK;
    }
    (void)diag_emit_error(ctx, "unavailable", "reboot_reason", NULL);
    return ESP_ERR_NOT_FOUND; /* logged as result=unavailable */
}
