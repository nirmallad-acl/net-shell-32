/**
 * @file devinfo_validate.h
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
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware revision: ^[A-Za-z0-9._-]{1,32}$ . NULL is invalid. */
bool devinfo_validate_hw_rev(const char *rev);

/* Firmware version: ^[A-Za-z0-9._+-]{1,max_len}$ and not the placeholder "1".
 * `max_len` is the maximum length in characters excluding the NUL. The string
 * is read with a hard bound of max_len + 1 bytes, so a non-terminated buffer is
 * never over-read; `bound` is the size of the buffer that holds it. */
bool devinfo_validate_fw_version(const char *ver, size_t bound, size_t max_len);

#ifdef __cplusplus
}
#endif
