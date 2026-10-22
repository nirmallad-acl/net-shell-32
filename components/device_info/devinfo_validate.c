/**
 * @file devinfo_validate.c
 * @brief PURE: hardware revision and firmware version character-set and length rules
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
 * REQ-FUNC-20, REQ-FUNC-25, REQ-FUNC-33.
 */
#include "devinfo_validate.h"

#include <string.h>

#define HW_REV_MAX 32u

static bool is_alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool devinfo_validate_hw_rev(const char *rev)
{
    if (rev == NULL) {
        return false;
    }
    size_t n = 0;
    for (; n <= HW_REV_MAX; n++) {
        char c = rev[n];
        if (c == '\0') {
            break;
        }
        if (!(is_alnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return n >= 1u && n <= HW_REV_MAX;
}

bool devinfo_validate_fw_version(const char *ver, size_t bound, size_t max_len)
{
    if (ver == NULL || bound == 0u) {
        return false;
    }
    size_t n = 0;
    for (; n < bound && n <= max_len; n++) {
        char c = ver[n];
        if (c == '\0') {
            break;
        }
        if (!(is_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-')) {
            return false;
        }
    }
    if (n == 0u || n > max_len) {
        return false;
    }
    if (n >= bound || ver[n] != '\0') {
        return false; /* not terminated within bound */
    }
    if (n == 1u && ver[0] == '1') {
        return false; /* IDF's placeholder when version.txt is absent */
    }
    return true;
}
