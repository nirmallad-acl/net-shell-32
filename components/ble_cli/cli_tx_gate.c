/**
 * @file cli_tx_gate.c
 * @brief PURE: BLE CLI TX/RX gate decisions, generation filter and input-overflow latch
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
 * REQ-FUNC-60, 61, 62, 64; tech_spec.md 2.5.2, 2.5.5.
 */
#include "cli_tx_gate.h"

#include <stddef.h>

cli_tx_verdict_t cli_tx_decide(uint32_t line_gen, uint32_t cur_secure_gen, uint32_t failed_gen, bool subscribed)
{
    if (cur_secure_gen == 0u || line_gen != cur_secure_gen) {
        return CLI_TX_DROP_STALE;
    }
    if (failed_gen != 0u && failed_gen == line_gen) {
        return CLI_TX_DROP_FAILED;
    }
    if (!subscribed) {
        return CLI_TX_DROP_UNSUBSCRIBED;
    }
    return CLI_TX_SEND;
}

bool cli_sink_admit(uint32_t ctx_gen, uint32_t cur_secure_gen, uint32_t failed_gen)
{
    return cur_secure_gen != 0u && ctx_gen == cur_secure_gen && !(failed_gen != 0u && failed_gen == ctx_gen);
}

bool cli_tx_failure_is_terminal(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE;
}

void cli_resync_init(cli_resync_t *r)
{
    if (r != NULL) {
        r->gen = 0;
        r->pending = false;
    }
}

void cli_resync_note_failure(cli_resync_t *r, uint32_t gen, size_t slices_sent, esp_err_t err)
{
    if (r == NULL) {
        return;
    }
    if (slices_sent > 0u && !cli_tx_failure_is_terminal(err)) {
        r->gen = gen;
        r->pending = true;
    }
}

bool cli_resync_take(cli_resync_t *r, uint32_t gen)
{
    if (r == NULL || !r->pending) {
        return false;
    }
    r->pending = false;
    return r->gen == gen;
}

bool cli_cmd_should_run(uint32_t item_gen, uint32_t cur_secure_gen)
{
    return item_gen != 0u && item_gen == cur_secure_gen;
}

cli_sec_verdict_t cli_rx_sec_verdict(bool encrypted, bool bonded)
{
    if (!encrypted) {
        return CLI_SEC_NEED_ENCRYPTION;
    }
    if (!bonded) {
        return CLI_SEC_NEED_BOND;
    }
    return CLI_SEC_OK;
}

void cli_latch_init(cli_latch_t *l)
{
    if (l != NULL) {
        atomic_store(&l->active, 0u);
        atomic_store(&l->gen, 0u);
    }
}

bool cli_latch_active(cli_latch_t *l)
{
    return l != NULL && atomic_load(&l->active) != 0u;
}

void cli_latch_clear(cli_latch_t *l)
{
    if (l != NULL) {
        atomic_store(&l->active, 0u);
    }
}

cli_rx_result_t cli_rx_offer(cli_latch_t *l, uint32_t gen, cli_push_fn push, void *ctx)
{
    if (l == NULL || push == NULL) {
        return CLI_RX_DROPPED_FULL;
    }
    if (atomic_load(&l->active) != 0u) {
        return CLI_RX_DROPPED_LATCHED;
    }
    if (push(ctx)) {
        return CLI_RX_QUEUED;
    }
    atomic_store(&l->gen, gen);
    atomic_store(&l->active, 1u);
    return CLI_RX_DROPPED_FULL;
}

bool cli_latch_ready_to_clear(cli_latch_t *l, bool cmd_queue_empty, uint32_t *gen_out)
{
    if (l == NULL || !cmd_queue_empty || atomic_load(&l->active) == 0u) {
        return false;
    }
    if (gen_out != NULL) {
        *gen_out = atomic_load(&l->gen);
    }
    return true;
}
