/**
 * @file cli_tx_gate.h
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
 */

/*
 * The decisions of ble_cli.c that need no FreeRTOS or NimBLE, extracted without
 * changing behaviour so they are unit-tested (test_cli_tx, test_gen_filter):
 * REQ-FUNC-60/61 (security and subscription gating), REQ-FUNC-62/64 (generation
 * filter), tech_spec.md 2.5.2 step 5 (input-overflow latch) and 2.5.5 (TX failure).
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- TX ---- */
typedef enum {
    CLI_TX_SEND = 0,
    CLI_TX_DROP_STALE,        /* link not secure, or the line belongs to another generation */
    CLI_TX_DROP_FAILED,       /* the generation's TX path is marked terminally failed */
    CLI_TX_DROP_UNSUBSCRIBED, /* the peer never enabled notifications: inert, nothing is sent */
} cli_tx_verdict_t;

/* cur_secure_gen is the generation of the current link if it is encrypted AND bonded,
 * else 0 (no link / not secure). */
cli_tx_verdict_t cli_tx_decide(uint32_t line_gen, uint32_t cur_secure_gen, uint32_t failed_gen, bool subscribed);

/* A response may be handed to the sink only for the current secure, non-failed generation. */
bool cli_sink_admit(uint32_t ctx_gen, uint32_t cur_secure_gen, uint32_t failed_gen);

/* Only "the link is gone / no longer secure" (ESP_ERR_INVALID_STATE) is terminal for a
 * generation; congestion (ESP_ERR_NO_MEM) and other transient errors are not (ADV-IMPL-05). */
bool cli_tx_failure_is_terminal(esp_err_t err);

/* A line that failed AFTER some of its slices were already sent leaves the peer with a
 * truncated line; the next line would be glued onto it. Remember that and send one resync
 * LF before the next line of the same generation (ADV-R2-01). Worker task only. */
typedef struct {
    uint32_t gen;     /* generation the pending resync belongs to */
    bool     pending;
} cli_resync_t;

void cli_resync_init(cli_resync_t *r);
/* slices_sent = slices of the failed line that were accepted before the failure. Sets
 * the pending flag only for a NON-terminal failure with slices_sent > 0. */
void cli_resync_note_failure(cli_resync_t *r, uint32_t gen, size_t slices_sent, esp_err_t err);
/* true exactly once if a resync LF must be sent before the next line of generation gen;
 * a pending resync of another generation is discarded. */
bool cli_resync_take(cli_resync_t *r, uint32_t gen);

/* ---- generation filter on the command side ---- */
/* A dequeued command runs only if its generation is the current secure one. */
bool cli_cmd_should_run(uint32_t item_gen, uint32_t cur_secure_gen);

/* ---- RX security check (REQ-FUNC-60) ---- */
typedef enum {
    CLI_SEC_OK = 0,
    CLI_SEC_NEED_ENCRYPTION, /* ATT 0x0F Insufficient Encryption */
    CLI_SEC_NEED_BOND,       /* ATT 0x05 Insufficient Authentication: encrypted but not bonded */
} cli_sec_verdict_t;
cli_sec_verdict_t cli_rx_sec_verdict(bool encrypted, bool bonded);

/* ---- input-overflow latch (tech_spec.md 2.5.2 step 5) ---- */
typedef struct {
    _Atomic uint32_t active;
    _Atomic uint32_t gen;
} cli_latch_t;

typedef enum {
    CLI_RX_QUEUED = 0,
    CLI_RX_DROPPED_LATCHED, /* the latch was already set: dropped to preserve order */
    CLI_RX_DROPPED_FULL,    /* the queue was full: dropped, the latch is now set */
} cli_rx_result_t;

typedef bool (*cli_push_fn)(void *ctx); /* try to enqueue one item without blocking */

void cli_latch_init(cli_latch_t *l);
bool cli_latch_active(cli_latch_t *l);
void cli_latch_clear(cli_latch_t *l);

/* Offers one parsed item. While the latch is set every later item is dropped (even if the
 * queue has room again), so a later command can never run ahead of an earlier dropped one. */
cli_rx_result_t cli_rx_offer(cli_latch_t *l, uint32_t gen, cli_push_fn push, void *ctx);

/* Worker side: true if the latch is set and the command queue is empty, i.e. exactly one
 * "ERR busy reason=input_overflow" must be emitted for generation *gen_out, after which
 * the caller clears the latch. */
bool cli_latch_ready_to_clear(cli_latch_t *l, bool cmd_queue_empty, uint32_t *gen_out);

#ifdef __cplusplus
}
#endif
