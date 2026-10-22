/**
 * @file diag_internal.h
 * @brief Private: handler prototypes and helpers shared inside diag_cmds
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
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "diag_cmds.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_TAG "diag"

/* esp_timer based milliseconds since boot (wraps after about 49 days; log use only). */
uint32_t    diag_now_ms(void);
const char *diag_transport_name(diag_transport_t t);

/* Writes one response line (without LF) through the context's sink. Validates
 * ctx and line. Handlers produce output ONLY through this plus diag_format.c
 * (REQ-FUNC-14/15): no printf/puts/ESP_LOG for response lines. */
esp_err_t diag_write_line(const diag_ctx_t *ctx, const char *line);

/* One handler per command (cmd_*.c). Return ESP_OK when an OK/DATA line was the
 * outcome, otherwise a non-OK code after an ERR line (or nothing, when the link
 * is gone). */
esp_err_t diag_cmd_hw_version(const diag_ctx_t *ctx);
esp_err_t diag_cmd_fw_version(const diag_ctx_t *ctx);
esp_err_t diag_cmd_reboot_reason(const diag_ctx_t *ctx);
esp_err_t diag_cmd_accel_xyz(const diag_ctx_t *ctx);
esp_err_t diag_cmd_reboot(const diag_ctx_t *ctx);

/* diag_serial.c */
esp_err_t diag_serial_register(void);

/* diag_resmon.c */
esp_err_t diag_resmon_init(void);

#ifdef __cplusplus
}
#endif
