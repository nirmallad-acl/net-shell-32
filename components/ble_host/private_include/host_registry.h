/**
 * @file host_registry.h
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
 */

/*
 * PURE module (tech_spec.md section 2.1): includes only standard headers,
 * esp_err.h and the public ble_host_client.h. No NimBLE, no FreeRTOS. The
 * unit of host testing (test_host_registry).
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble_host_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_REG_ABS_MAX 4 /* upper bound of CONFIG_BLE_HOST_MAX_CLIENTS (range 2..4) */

/* Bits of the pending-action word shared by the esp_timer callbacks and the
 * host task (tech_spec.md section 2.3.5). */
#define HOST_PEND_ADV_RETRY    (1u << 0)
#define HOST_PEND_SEC_DEADLINE (1u << 1)
#define HOST_PEND_SEC_RETRY    (1u << 2)

typedef struct {
    ble_host_client_cfg_t cfg[HOST_REG_ABS_MAX];
    uint8_t  count;
    uint8_t  cap;
    int8_t   default_id;   /* -1 if none */
    int8_t   primary_id;   /* client whose UUID is in the ADV data; -1 = default */
    uint32_t gen;          /* connection generation, never 0 once started */
    uint32_t engaged_mask; /* bit i: client i engaged on the current link */
} host_reg_t;

/* cap is clamped to 1..HOST_REG_ABS_MAX. Zeroes the table, gen starts at 1. */
void host_reg_init(host_reg_t *r, uint8_t cap);

/* accepting == false means "not in state INIT": ESP_ERR_INVALID_STATE. Errors:
 * ESP_ERR_INVALID_ARG on NULL/duplicate name or svcs or a missing adv UUID or a
 * second default_primary, ESP_ERR_NO_MEM over capacity. Every error leaves the
 * table unchanged. */
esp_err_t host_reg_register(host_reg_t *r, bool accepting, const ble_host_client_cfg_t *cfg,
                            ble_host_client_id_t *out_id);

uint8_t host_reg_count(const host_reg_t *r);
/* NULL if id is out of range. */
const ble_host_client_cfg_t *host_reg_get(const host_reg_t *r, uint8_t id);

/* Generation counter: increments by 1 and skips 0 on wrap. */
uint32_t host_reg_next_gen(host_reg_t *r);
uint32_t host_reg_gen(const host_reg_t *r);

/* Advertising primary: the client that most recently asked for it, else the
 * default_primary client. Returns -1 if neither exists. */
int host_reg_primary(const host_reg_t *r);
esp_err_t host_reg_set_primary(host_reg_t *r, uint8_t id, bool primary);

/* Engagement (REQ-FUNC-76). note_engaged sets the bit only if gen == current. */
void host_reg_clear_engaged(host_reg_t *r);
void host_reg_note_engaged(host_reg_t *r, uint8_t id, uint32_t gen);
/* true if a link is up, gen is current, bit id is set and no other bit is set. */
bool host_reg_release_allowed(const host_reg_t *r, uint8_t id, uint32_t gen, bool link_up);

/* Calls every registered callback in registration order. */
void host_reg_dispatch(const host_reg_t *r, const ble_host_evt_t *evt);
/* Same, and records the longest callback time of non-waived clients in *max_us. */
void host_reg_dispatch_timed(const host_reg_t *r, const ble_host_evt_t *evt,
                             int64_t (*now_us)(void), uint32_t *max_us);

/* Security deadline decision (tech_spec.md section 2.3.9): terminate only if a
 * link is up, its generation is the one the deadline was armed for, and it is
 * still not secure. */
bool host_deadline_should_terminate(bool link_up, uint32_t cur_gen, uint32_t deadline_gen, bool secure);

/* Pending-action word helpers (flag is the source of truth, the kick event is
 * only a wake-up). */
void     host_pending_set(_Atomic uint32_t *word, uint32_t bits);
uint32_t host_pending_take(_Atomic uint32_t *word);

#ifdef __cplusplus
}
#endif
