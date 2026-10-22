/**
 * @file cli_frag.c
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
 *
 * REQ-FUNC-09: a line is split into slices of at most ATT_MTU - 3 bytes.
 */
#include "cli_frag.h"

esp_err_t cli_frag_plan(size_t total, size_t max_payload, cli_frag_plan_t *plan)
{
    if (plan == NULL || total == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_payload < CLI_FRAG_MIN_PAYLOAD) {
        max_payload = CLI_FRAG_MIN_PAYLOAD;
    }
    plan->total = total;
    plan->max_payload = max_payload;
    plan->count = total / max_payload + ((total % max_payload) != 0u ? 1u : 0u);
    return ESP_OK;
}

size_t cli_frag_slice(const cli_frag_plan_t *plan, size_t idx, size_t *offset)
{
    if (offset != NULL) {
        *offset = 0;
    }
    if (plan == NULL || idx >= plan->count) {
        return 0;
    }
    size_t off = idx * plan->max_payload;
    size_t len = plan->total - off;
    if (len > plan->max_payload) {
        len = plan->max_payload;
    }
    if (offset != NULL) {
        *offset = off;
    }
    return len;
}
