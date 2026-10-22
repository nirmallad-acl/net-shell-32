/**
 * @file ble_ota_gate.c
 * @brief PURE: OTA arm state machine, RX/TX gates, on_connect-once and reboot guard slots
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
 * REQ-FUNC-31, 32, 75, 77, 79; tech_spec.md 2.4.3 and 2.4.4 (INV-A1..A9,
 * INV-R1). Every write of the arm state is a CAS; every guard slot transition
 * is a CAS on a word that carries the request ticket, so exactly one of the
 * guard's commit and the caller's closing CAS wins.
 */
#include "ble_ota_gate.h"

#include <stdatomic.h>
#include <string.h>

#include "ble_ota.h"

/* ---- arm state ---- */
static _Atomic uint32_t s_arm_state = OTA_ARM_DISARMED;
static _Atomic uint32_t s_arm_epoch;
static _Atomic(const char *) s_disarm_reason;
static _Atomic uint32_t s_aborted_at_ms; /* 0 = not set (R3-03) */
static uint32_t         s_on_connect_gen; /* host task only */

static const char s_reason_none[] = "none";

void ble_ota_gate_reset(void)
{
    atomic_store(&s_arm_state, (uint32_t)OTA_ARM_DISARMED);
    atomic_store(&s_arm_epoch, 0u);
    atomic_store(&s_disarm_reason, s_reason_none);
    atomic_store(&s_aborted_at_ms, 0u);
    s_on_connect_gen = 0;
}

ota_arm_state_t ble_ota_gate_state(void)
{
    return (ota_arm_state_t)atomic_load(&s_arm_state);
}

uint32_t ble_ota_gate_epoch(void)
{
    return atomic_load(&s_arm_epoch);
}

static bool cas_state(ota_arm_state_t from, ota_arm_state_t to)
{
    uint32_t exp = (uint32_t)from;
    return atomic_compare_exchange_strong(&s_arm_state, &exp, (uint32_t)to);
}

bool ble_ota_gate_try_arming(void)
{
    if (!cas_state(OTA_ARM_DISARMED, OTA_ARM_ARMING)) {
        return false;
    }
    /* Bumped while the state is still ARMING, so the TX gate drops every frame:
     * ARMED can never be visible with an older frame's epoch current (INV-A8). */
    (void)atomic_fetch_add(&s_arm_epoch, 1u);
    return true;
}

bool ble_ota_gate_cancel_arming(void)
{
    return cas_state(OTA_ARM_ARMING, OTA_ARM_DISARMED);
}

int32_t ble_ota_gate_arm_commit(void)
{
    if (!cas_state(OTA_ARM_ARMING, OTA_ARM_ARMED)) {
        return OTA_EXEC_NOT_CLAIMED;
    }
    s_on_connect_gen = 0;
    return OTA_EXEC_CLAIMED;
}

int32_t ble_ota_gate_disarm_commit(const char *reason, uint32_t expiry_epoch, bool link_up,
                                   uint32_t readv_epoch, uint32_t now_ms)
{
    if (ble_ota_gate_state() != OTA_ARM_ARMED) {
        return OTA_EXEC_NOT_CLAIMED; /* decisive check: nothing written */
    }
    if (expiry_epoch != 0u && (link_up || readv_epoch != expiry_epoch)) {
        return OTA_EXEC_STALE; /* the peer reconnected: a stale expiry has no effect at all */
    }
    /* Fields first, then the publishing CAS: a janitor that sees ABORTED always
     * sees a non-zero timestamp and the original reason (R3-03). */
    atomic_store(&s_disarm_reason, (reason != NULL) ? reason : s_reason_none);
    atomic_store(&s_aborted_at_ms, (now_ms != 0u) ? now_ms : 1u);
    if (!cas_state(OTA_ARM_ARMED, OTA_ARM_ABORTED)) {
        return OTA_EXEC_NOT_CLAIMED; /* unreachable: only host-task fns leave ARMED */
    }
    (void)atomic_fetch_add(&s_arm_epoch, 1u);
    s_on_connect_gen = 0;
    return OTA_EXEC_CLAIMED;
}

bool ble_ota_gate_claim_drain(ota_arm_state_t from)
{
    if (from != OTA_ARM_ABORTED && from != OTA_ARM_DRAIN_STUCK) {
        return false;
    }
    return cas_state(from, OTA_ARM_DISARMING);
}

void ble_ota_gate_drain_done(bool ok)
{
    (void)cas_state(OTA_ARM_DISARMING, ok ? OTA_ARM_DISARMED : OTA_ARM_DRAIN_STUCK);
}

const char *ble_ota_gate_disarm_reason(void)
{
    const char *r = atomic_load(&s_disarm_reason);
    return (r != NULL) ? r : s_reason_none;
}

void ble_ota_gate_clear_aborted_at(void)
{
    atomic_store(&s_aborted_at_ms, 0u);
}

bool ble_ota_gate_janitor_aborted_due(uint32_t now_ms)
{
    if (ble_ota_gate_state() != OTA_ARM_ABORTED) {
        return false;
    }
    uint32_t at = atomic_load(&s_aborted_at_ms);
    if (at == 0u) {
        return false; /* never claim an ABORTED whose timestamp is not set */
    }
    return (uint32_t)(now_ms - at) > OTA_ABORTED_JANITOR_AGE_MS;
}

bool ble_ota_gate_link_loss_needs_abort(void)
{
    return ble_ota_gate_state() != OTA_ARM_DISARMED;
}

bool ble_ota_gate_on_connect_due(bool secure, bool subscribed, uint32_t gen)
{
    if (ble_ota_gate_state() != OTA_ARM_ARMED || !secure || !subscribed || gen == 0u ||
        s_on_connect_gen == gen) {
        return false;
    }
    s_on_connect_gen = gen;
    return true;
}

void ble_ota_gate_on_connect_reset(void)
{
    s_on_connect_gen = 0;
}

/* ---- RX / TX gates ---- */

ota_rx_verdict_t ble_ota_gate_rx_decision(void)
{
    if (ble_ota_gate_reboot_pending()) {
        return OTA_RX_REJECT_REBOOT_PENDING;
    }
    if (ble_ota_gate_state() != OTA_ARM_ARMED) {
        return OTA_RX_REJECT_NOT_ARMED;
    }
    return OTA_RX_FORWARD;
}

ota_tx_verdict_t ble_ota_gate_tx_check(const ota_tx_in_t *in)
{
    if (in == NULL) {
        return OTA_TX_NOT_ARMED;
    }
    if (ble_ota_gate_state() != OTA_ARM_ARMED) {
        return OTA_TX_NOT_ARMED;
    }
    if (in->item_epoch != ble_ota_gate_epoch()) {
        return OTA_TX_STALE_EPOCH;
    }
    if (!in->link_present) {
        return OTA_TX_NO_LINK;
    }
    if (in->item_gen != in->cur_gen) {
        return OTA_TX_STALE_GEN;
    }
    if (!in->subscribed) {
        return OTA_TX_NOT_SUBSCRIBED;
    }
    if (!in->secure) {
        return OTA_TX_NOT_SECURE;
    }
    return OTA_TX_OK;
}

const char *ble_ota_gate_tx_reason(ota_tx_verdict_t v)
{
    switch (v) {
    case OTA_TX_NOT_ARMED:      return "not_armed";
    case OTA_TX_STALE_EPOCH:    return "stale_epoch";
    case OTA_TX_NO_LINK:        return "no_link";
    case OTA_TX_STALE_GEN:      return "stale_gen";
    case OTA_TX_NOT_SUBSCRIBED: return "not_subscribed";
    case OTA_TX_NOT_SECURE:     return "not_secure";
    default:                    return "ok";
    }
}

/* ---- reboot guard slots ---- */

typedef enum {
    GUARD_NONE = 0,
    GUARD_COMMITTED = 1,
    GUARD_CANCELLED = 2,
    GUARD_COMMITTED_OVR = 3, /* committed using the G2-07 drain-stuck exception */
} guard_st_t;

#define SLOT_MAKE(ticket, st) ((uint32_t)(((ticket) << 2) | (uint32_t)(st)))
#define SLOT_ST(w)            ((guard_st_t)((w) & 3u))
#define TICKET_MASK           0x3FFFFFFFu

/* (ticket << 2) | guard_st_t; the boot value is CANCELLED (no request open). */
static _Atomic uint32_t s_guard_slot[2] = { GUARD_CANCELLED, GUARD_CANCELLED };
static uint32_t         s_guard_gen[2];   /* written before the slot is published */
static _Atomic uint32_t s_guard_ticket;
static const ble_ota_guard_ops_t *s_ops;

void ble_ota_gate_set_guard_ops(const ble_ota_guard_ops_t *ops)
{
    s_ops = ops;
}

void ble_ota_gate_reset_guard(void)
{
    atomic_store(&s_guard_slot[0], (uint32_t)GUARD_CANCELLED);
    atomic_store(&s_guard_slot[1], (uint32_t)GUARD_CANCELLED);
    s_guard_gen[0] = 0;
    s_guard_gen[1] = 0;
}

static uint32_t next_ticket(void)
{
    uint32_t t;
    do {
        t = (atomic_fetch_add(&s_guard_ticket, 1u) + 1u) & TICKET_MASK;
    } while (t == 0u);
    return t;
}

bool ble_ota_gate_reboot_pending(void)
{
    for (unsigned i = 0; i < 2; i++) {
        guard_st_t st = SLOT_ST(atomic_load(&s_guard_slot[i]));
        if (st == GUARD_COMMITTED || st == GUARD_COMMITTED_OVR) {
            return true;
        }
    }
    return false;
}

/* Checks 3 and 4 of the guard; shared by guard_fn and the direct path. */
static int32_t eval_checks(const ble_ota_guard_ops_t *ops, bool *override)
{
    *override = false;
    if (!ops->proto_idle()) {
        /* G2-07: a stuck drain can only be in the inactive slot, so reboot is accepted. */
        if (ble_ota_gate_state() == OTA_ARM_DRAIN_STUCK || ops->proto_drain_stuck()) {
            *override = true;
        } else {
            return OTA_GUARD_OTA_ACTIVE;
        }
    }
    if (ops->pending_verify()) {
        return OTA_GUARD_PENDING_VERIFY; /* not relaxed by G2-07: a restart here is a rollback */
    }
    return OTA_GUARD_COMMITTED;
}

int32_t ble_ota_gate_guard_fn(uintptr_t arg)
{
    const uint32_t t = (uint32_t)(arg >> 1);
    const unsigned idx = (unsigned)(arg & 1u);
    const ble_ota_guard_ops_t *ops = s_ops;
    if (ops == NULL) {
        return OTA_GUARD_CANCELLED;
    }
    const uint32_t open = SLOT_MAKE(t, GUARD_NONE);
    if (atomic_load_explicit(&s_guard_slot[idx], memory_order_acquire) != open) {
        return OTA_GUARD_CANCELLED; /* caller already closed, or a newer ticket: no effect */
    }
    const uint32_t gen = s_guard_gen[idx];
    if (gen != 0u && !ops->gen_current_secure(gen)) {
        return OTA_GUARD_STALE;
    }
    bool override = false;
    int32_t chk = eval_checks(ops, &override);
    if (chk != OTA_GUARD_COMMITTED) {
        return chk;
    }
    uint32_t exp = open;
    if (atomic_compare_exchange_strong(&s_guard_slot[idx], &exp,
                                       SLOT_MAKE(t, override ? GUARD_COMMITTED_OVR : GUARD_COMMITTED))) {
        return OTA_GUARD_COMMITTED;
    }
    return OTA_GUARD_CANCELLED; /* lost to the caller's closing CAS: no effect at all */
}

/* Host not UP: no link and no OTA RX can exist, and guard_fn for this ticket was
 * never posted. Commit first, then check (INV-R1); revert only on our own ticket. */
static esp_err_t direct_path(const ble_ota_guard_ops_t *ops, unsigned idx, uint32_t t, uint32_t ble_gen,
                             bool *out_override)
{
    uint32_t open = SLOT_MAKE(t, GUARD_NONE);
    if (ble_gen != 0u) {
        /* A BLE request cannot have a current link before the host is up. */
        (void)atomic_compare_exchange_strong(&s_guard_slot[idx], &open, SLOT_MAKE(t, GUARD_CANCELLED));
        return ESP_ERR_BLE_OTA_REBOOT_STALE;
    }
    if (!atomic_compare_exchange_strong(&s_guard_slot[idx], &open, SLOT_MAKE(t, GUARD_COMMITTED))) {
        return ESP_ERR_INVALID_STATE;
    }
    bool override = false;
    int32_t chk = eval_checks(ops, &override);
    if (chk != OTA_GUARD_COMMITTED) {
        uint32_t committed = SLOT_MAKE(t, GUARD_COMMITTED);
        (void)atomic_compare_exchange_strong(&s_guard_slot[idx], &committed, SLOT_MAKE(t, GUARD_CANCELLED));
        return (chk == OTA_GUARD_PENDING_VERIFY) ? ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY
                                                 : ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE;
    }
    if (out_override != NULL) {
        *out_override = override;
    }
    return ESP_OK;
}

esp_err_t ble_ota_gate_guard_commit(uint32_t ble_gen, uint32_t timeout_ms, bool *out_override)
{
    const ble_ota_guard_ops_t *ops = s_ops;
    if (out_override != NULL) {
        *out_override = false;
    }
    if (ops == NULL || ops->exec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned idx = (ble_gen != 0u) ? 1u : 0u;
    const uint32_t t = next_ticket();

    uint32_t w = atomic_load(&s_guard_slot[idx]);
    if (SLOT_ST(w) != GUARD_CANCELLED) {
        return ESP_ERR_INVALID_STATE; /* a request of this transport is still open */
    }
    s_guard_gen[idx] = ble_gen; /* before publication */
    const uint32_t open = SLOT_MAKE(t, GUARD_NONE);
    if (!atomic_compare_exchange_strong(&s_guard_slot[idx], &w, open)) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t res = -1;
    esp_err_t rc = ops->exec(ble_ota_gate_guard_fn, (uintptr_t)((t << 1) | idx), timeout_ms, &res);
    if (rc == ESP_ERR_BLE_HOST_NOT_UP) {
        return direct_path(ops, idx, t, ble_gen, out_override);
    }

    /* Closing CAS after every other outcome: exactly one of {guard_fn's commit,
     * this CAS} wins, so "committed" and "the caller restarts" are the same event. */
    uint32_t exp = open;
    if (atomic_compare_exchange_strong(&s_guard_slot[idx], &exp, SLOT_MAKE(t, GUARD_CANCELLED))) {
        /* Won: not committed, and a late guard_fn can never commit this ticket. */
        if (rc == ESP_OK) {
            switch (res) {
            case OTA_GUARD_STALE:          return ESP_ERR_BLE_OTA_REBOOT_STALE;
            case OTA_GUARD_OTA_ACTIVE:     return ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE;
            case OTA_GUARD_PENDING_VERIFY: return ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY;
            default:                       return ESP_FAIL; /* COMMITTED/CANCELLED here is impossible */
            }
        }
        if (rc == ESP_ERR_TIMEOUT || rc == ESP_ERR_BLE_HOST_EXEC_INDETERMINATE) {
            return ESP_ERR_TIMEOUT;
        }
        return rc; /* WRONG_TASK and any other code pass through */
    }
    /* Lost: the slot holds (t, COMMITTED*): the request IS committed, whatever rc was. */
    if (out_override != NULL) {
        *out_override = (SLOT_ST(exp) == GUARD_COMMITTED_OVR);
    }
    return ESP_OK;
}
