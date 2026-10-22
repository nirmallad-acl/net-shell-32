/**
 * @file adv_payload.h
 * @brief PURE: raw ADV / scan-response byte builder
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

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADV_PAYLOAD_MAX 31 /* legacy advertising data / scan response limit */

/* ADV data: Flags AD (02 01 06) + Complete List of 128-bit UUIDs AD
 * (11 07 <16 bytes, little-endian>) = 21 bytes. Byte-identical to what the
 * NETSHELL32-2 ble_hs_adv_fields produced. ESP_ERR_INVALID_ARG on NULL,
 * ESP_ERR_INVALID_SIZE if cap is too small. */
esp_err_t adv_build_adv(uint8_t *buf, size_t cap, size_t *out_len, const uint8_t *primary_uuid128);

/* Scan response: Complete Local Name AD + one Complete List of 128-bit UUIDs AD
 * holding n_uuids UUIDs (omitted when n_uuids == 0). ESP_ERR_INVALID_SIZE if more
 * than ADV_PAYLOAD_MAX (or cap) bytes would be needed. */
esp_err_t adv_build_rsp(uint8_t *buf, size_t cap, size_t *out_len, const char *name,
                        const uint8_t *const *uuids128, size_t n_uuids);

#ifdef __cplusplus
}
#endif
