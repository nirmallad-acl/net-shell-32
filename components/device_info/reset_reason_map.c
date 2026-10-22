/**
 * @file reset_reason_map.c
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
 *
 * REQ-FUNC-27, REQ-FUNC-28. Table indexed by esp_reset_reason_t as in
 * IDF v6.0.2 esp_system.h (tech_spec.md 2.8.3).
 */
#include "reset_reason_map.h"

static const char *const s_tokens[RESET_REASON_MAP_COUNT] = {
    "unknown",      /* 0  ESP_RST_UNKNOWN */
    "power_on",     /* 1  ESP_RST_POWERON */
    "external",     /* 2  ESP_RST_EXT */
    "software",     /* 3  ESP_RST_SW */
    "panic",        /* 4  ESP_RST_PANIC */
    "int_wdt",      /* 5  ESP_RST_INT_WDT */
    "task_wdt",     /* 6  ESP_RST_TASK_WDT */
    "wdt",          /* 7  ESP_RST_WDT */
    "deepsleep",    /* 8  ESP_RST_DEEPSLEEP */
    "brownout",     /* 9  ESP_RST_BROWNOUT */
    "sdio",         /* 10 ESP_RST_SDIO */
    "usb",          /* 11 ESP_RST_USB */
    "jtag",         /* 12 ESP_RST_JTAG */
    "efuse",        /* 13 ESP_RST_EFUSE */
    "power_glitch", /* 14 ESP_RST_PWR_GLITCH */
    "cpu_lockup",   /* 15 ESP_RST_CPU_LOCKUP */
};

const char *reset_reason_token(int code)
{
    if (code < 0 || code >= RESET_REASON_MAP_COUNT) {
        return s_tokens[0];
    }
    return s_tokens[code];
}
