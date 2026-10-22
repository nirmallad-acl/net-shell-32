/**
 * @file cli_frag.h
 * @brief PURE: fragmentation plan of one response line for a given notification payload size
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

#define CLI_FRAG_MIN_PAYLOAD 20u /* ATT_MTU 23 - 3 */

typedef struct {
    size_t total;       /* line length including the terminating LF */
    size_t max_payload; /* bytes per notification (ATT_MTU - 3, at least 20) */
    size_t count;       /* number of slices */
} cli_frag_plan_t;

/* ESP_ERR_INVALID_ARG on NULL or total == 0. max_payload below 20 is raised to 20. */
esp_err_t cli_frag_plan(size_t total, size_t max_payload, cli_frag_plan_t *plan);

/* Slice idx (0-based): returns its length and stores its offset; 0 and offset 0
 * for idx >= count. Concatenating all slices reproduces the line byte for byte. */
size_t cli_frag_slice(const cli_frag_plan_t *plan, size_t idx, size_t *offset);

#ifdef __cplusplus
}
#endif
