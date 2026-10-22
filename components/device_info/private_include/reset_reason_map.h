/**
 * @file reset_reason_map.h
 * @brief PURE: esp_reset_reason_t code to token table, bounds-safe
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

#ifdef __cplusplus
extern "C" {
#endif

/* Number of mapped codes (ESP_RST_UNKNOWN .. ESP_RST_CPU_LOCKUP). device_info.c
 * ties this to the IDF enum with a _Static_assert. */
#define RESET_REASON_MAP_COUNT 16

/* Never NULL; any code outside 0..RESET_REASON_MAP_COUNT-1 gives "unknown". */
const char *reset_reason_token(int code);

#ifdef __cplusplus
}
#endif
