/**
 * @file host_registry.c
 * @brief PURE: client registration table, dispatch order, generation counter, engagement
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
 * REQ-FUNC-64 (generation), REQ-FUNC-71 (registration), REQ-FUNC-72
 * (dispatch order), REQ-FUNC-76 (engagement and link release).
 */
#include "host_registry.h"

#include <string.h>

void host_reg_init(host_reg_t *r, uint8_t cap)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    if (cap < 1) {
        cap = 1;
    }
    if (cap > HOST_REG_ABS_MAX) {
        cap = HOST_REG_ABS_MAX;
    }
    r->cap = cap;
    r->default_id = -1;
    r->primary_id = -1;
    r->gen = 1;
}

esp_err_t host_reg_register(host_reg_t *r, bool accepting, const ble_host_client_cfg_t *cfg,
                            ble_host_client_id_t *out_id)
{
    if (r == NULL || cfg == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!accepting) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->name == NULL || cfg->name[0] == '\0' || cfg->svcs == NULL ||
        cfg->adv_uuid128 == NULL || cfg->cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < r->count; i++) {
        if (strcmp(r->cfg[i].name, cfg->name) == 0 || r->cfg[i].svcs == cfg->svcs) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (cfg->default_primary && r->default_id >= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r->count >= r->cap) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t id = r->count;
    r->cfg[id] = *cfg;
    r->count = (uint8_t)(r->count + 1u);
    if (cfg->default_primary) {
        r->default_id = (int8_t)id;
    }
    *out_id = id;
    return ESP_OK;
}

uint8_t host_reg_count(const host_reg_t *r)
{
    return (r != NULL) ? r->count : 0;
}

const ble_host_client_cfg_t *host_reg_get(const host_reg_t *r, uint8_t id)
{
    if (r == NULL || id >= r->count) {
        return NULL;
    }
    return &r->cfg[id];
}

uint32_t host_reg_next_gen(host_reg_t *r)
{
    if (r == NULL) {
        return 0;
    }
    r->gen++;
    if (r->gen == 0) {
        r->gen = 1; /* 0 is the "no link" sentinel */
    }
    return r->gen;
}

uint32_t host_reg_gen(const host_reg_t *r)
{
    return (r != NULL) ? r->gen : 0;
}

int host_reg_primary(const host_reg_t *r)
{
    if (r == NULL) {
        return -1;
    }
    if (r->primary_id >= 0) {
        return r->primary_id;
    }
    return r->default_id;
}

esp_err_t host_reg_set_primary(host_reg_t *r, uint8_t id, bool primary)
{
    if (r == NULL || id >= r->count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (primary) {
        r->primary_id = (int8_t)id;
    } else if (r->primary_id == (int8_t)id) {
        r->primary_id = -1; /* back to the default_primary client */
    }
    return ESP_OK;
}

void host_reg_clear_engaged(host_reg_t *r)
{
    if (r != NULL) {
        r->engaged_mask = 0;
    }
}

void host_reg_note_engaged(host_reg_t *r, uint8_t id, uint32_t gen)
{
    if (r == NULL || id >= r->count || gen != r->gen) {
        return;
    }
    r->engaged_mask |= (1u << id);
}

bool host_reg_release_allowed(const host_reg_t *r, uint8_t id, uint32_t gen, bool link_up)
{
    if (r == NULL || id >= r->count || !link_up || gen != r->gen) {
        return false;
    }
    return r->engaged_mask == (1u << id);
}

void host_reg_dispatch_timed(const host_reg_t *r, const ble_host_evt_t *evt,
                             int64_t (*now_us)(void), uint32_t *max_us)
{
    if (r == NULL || evt == NULL) {
        return;
    }
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->cfg[i].cb == NULL) {
            continue;
        }
        int64_t t0 = (now_us != NULL) ? now_us() : 0;
        r->cfg[i].cb(evt, r->cfg[i].user);
        if (now_us != NULL && max_us != NULL && !r->cfg[i].waive_timing) {
            int64_t dt = now_us() - t0;
            if (dt < 0) {
                dt = 0;
            }
            if (dt > (int64_t)UINT32_MAX) {
                dt = (int64_t)UINT32_MAX;
            }
            if ((uint32_t)dt > *max_us) {
                *max_us = (uint32_t)dt;
            }
        }
    }
}

void host_reg_dispatch(const host_reg_t *r, const ble_host_evt_t *evt)
{
    host_reg_dispatch_timed(r, evt, NULL, NULL);
}

bool host_deadline_should_terminate(bool link_up, uint32_t cur_gen, uint32_t deadline_gen, bool secure)
{
    return link_up && cur_gen == deadline_gen && cur_gen != 0 && !secure;
}

void host_pending_set(_Atomic uint32_t *word, uint32_t bits)
{
    if (word != NULL) {
        atomic_fetch_or(word, bits);
    }
}

uint32_t host_pending_take(_Atomic uint32_t *word)
{
    if (word == NULL) {
        return 0;
    }
    return atomic_exchange(word, 0u);
}
