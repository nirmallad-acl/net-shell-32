/**
 * @file adv_payload.c
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
 *
 * REQ-FUNC-78 (advertising payloads).
 */
#include "adv_payload.h"

#include <string.h>

#define AD_TYPE_FLAGS        0x01u
#define AD_TYPE_UUID128_FULL 0x07u
#define AD_TYPE_NAME_FULL    0x09u
#define AD_FLAGS_VALUE       0x06u /* LE General Discoverable | BR/EDR not supported */

esp_err_t adv_build_adv(uint8_t *buf, size_t cap, size_t *out_len, const uint8_t *primary_uuid128)
{
    if (buf == NULL || out_len == NULL || primary_uuid128 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t need = 3u + 2u + 16u;
    if (cap < need || need > ADV_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 2u;
    buf[1] = AD_TYPE_FLAGS;
    buf[2] = AD_FLAGS_VALUE;
    buf[3] = 17u;
    buf[4] = AD_TYPE_UUID128_FULL;
    memcpy(&buf[5], primary_uuid128, 16);
    *out_len = need;
    return ESP_OK;
}

esp_err_t adv_build_rsp(uint8_t *buf, size_t cap, size_t *out_len, const char *name,
                        const uint8_t *const *uuids128, size_t n_uuids)
{
    if (buf == NULL || out_len == NULL || name == NULL || (n_uuids > 0 && uuids128 == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_len = strlen(name);
    if (name_len > ADV_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t need = 2u + name_len;
    if (n_uuids > 0) {
        if (n_uuids > 4) {
            return ESP_ERR_INVALID_SIZE; /* also keeps the length arithmetic trivially safe */
        }
        need += 2u + 16u * n_uuids;
    }
    if (need > ADV_PAYLOAD_MAX || need > cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t pos = 0;
    buf[pos++] = (uint8_t)(1u + name_len);
    buf[pos++] = AD_TYPE_NAME_FULL;
    memcpy(&buf[pos], name, name_len);
    pos += name_len;
    if (n_uuids > 0) {
        buf[pos++] = (uint8_t)(1u + 16u * n_uuids);
        buf[pos++] = AD_TYPE_UUID128_FULL;
        for (size_t i = 0; i < n_uuids; i++) {
            if (uuids128[i] == NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(&buf[pos], uuids128[i], 16);
            pos += 16;
        }
    }
    *out_len = pos;
    return ESP_OK;
}
