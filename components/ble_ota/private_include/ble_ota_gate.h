/**
 * @file ble_ota_gate.h
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
 */

/*
 * NETSHELL32-5, tech_spec.md sections 2.4.3 and 2.4.4. PURE: no NimBLE, no
 * FreeRTOS, no esp_timer. Time, the protocol state and the host exec primitive
 * are injected so the decision logic is unit-tested on a PC (test_ota_gate).
 * R-07 / assumption A-01: the reboot guard's atomicity relies on every
 * IDLE -> non-IDLE protocol transition happening inside ble_ota_proto_rx() on
 * the NimBLE host task (verified at commit cd1798e, ble_ota_proto.c lines 363
 * and 615). Re-verify if ble_ota_proto.c ever changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble_host_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- arm state machine ---- */
typedef enum {
    OTA_ARM_DISARMED = 0,
    OTA_ARM_ARMING,
    OTA_ARM_ARMED,
    OTA_ARM_ABORTED,    /* disarm_fn left ARMED; no task has claimed the drain yet */
    OTA_ARM_DISARMING,  /* exactly one task owns the drain */
    OTA_ARM_DRAIN_STUCK,
} ota_arm_state_t;

/* Exec-function results (returned BY VALUE through ble_host_exec). */
#define OTA_EXEC_NOT_CLAIMED 0
#define OTA_EXEC_CLAIMED     1
#define OTA_EXEC_STALE       2

#define OTA_ABORTED_JANITOR_AGE_MS 1000u

void            ble_ota_gate_reset(void); /* boot state; also used by the unit tests */
ota_arm_state_t ble_ota_gate_state(void);
uint32_t        ble_ota_gate_epoch(void);

/* DISARMED -> ARMING by CAS; on success the arm epoch is bumped at once (R3-04). */
bool ble_ota_gate_try_arming(void);
/* ARMING -> DISARMED by CAS (start failure / exec TIMEOUT). true if won. */
bool ble_ota_gate_cancel_arming(void);
/* arm_fn core: CAS ARMING -> ARMED. Returns OTA_EXEC_CLAIMED or OTA_EXEC_NOT_CLAIMED
 * with no side effect. On success on_connect_gen is reset to 0. */
int32_t ble_ota_gate_arm_commit(void);
/* disarm_fn core (host task). (1) state != ARMED -> NOT_CLAIMED. (2) expiry_epoch != 0:
 * link_up or readv_epoch != expiry_epoch -> STALE. (3) record reason and aborted_at_ms
 * (never 0) BEFORE (4) the CAS ARMED -> ABORTED, then bump the epoch and reset
 * on_connect_gen. Returns CLAIMED only if the CAS was won. */
int32_t ble_ota_gate_disarm_commit(const char *reason, uint32_t expiry_epoch, bool link_up,
                                   uint32_t readv_epoch, uint32_t now_ms);
/* Drain claim: ABORTED or DRAIN_STUCK -> DISARMING by CAS. */
bool ble_ota_gate_claim_drain(ota_arm_state_t from);
/* Drain claimant only: DISARMING -> DISARMED (ok) or DRAIN_STUCK (!ok). */
void ble_ota_gate_drain_done(bool ok);
const char *ble_ota_gate_disarm_reason(void); /* never NULL */
/* Clears the janitor timestamp (done by the claimant of an ABORTED drain). */
void ble_ota_gate_clear_aborted_at(void);
/* true if state is ABORTED with a non-zero timestamp older than the janitor age. */
bool ble_ota_gate_janitor_aborted_due(uint32_t now_ms);
/* Link loss policy: abort whenever the state is not DISARMED. */
bool ble_ota_gate_link_loss_needs_abort(void);

/* on_connect exactly once per generation (host task only). */
bool ble_ota_gate_on_connect_due(bool secure, bool subscribed, uint32_t gen);
void ble_ota_gate_on_connect_reset(void);

/* ---- RX gate ---- */
typedef enum {
    OTA_RX_FORWARD = 0,
    OTA_RX_REJECT_REBOOT_PENDING,
    OTA_RX_REJECT_NOT_ARMED,
} ota_rx_verdict_t;
ota_rx_verdict_t ble_ota_gate_rx_decision(void);

/* ---- TX gate (evaluated per frame at dequeue) ---- */
typedef struct {
    uint32_t item_gen;
    uint32_t item_epoch;
    bool     link_present;
    uint32_t cur_gen;
    bool     subscribed;
    bool     secure;
} ota_tx_in_t;
typedef enum {
    OTA_TX_OK = 0,
    OTA_TX_NOT_ARMED,
    OTA_TX_STALE_EPOCH,
    OTA_TX_NO_LINK,
    OTA_TX_STALE_GEN,
    OTA_TX_NOT_SUBSCRIBED,
    OTA_TX_NOT_SECURE,
} ota_tx_verdict_t;
ota_tx_verdict_t ble_ota_gate_tx_check(const ota_tx_in_t *in);
const char      *ble_ota_gate_tx_reason(ota_tx_verdict_t v);

/* ---- reboot guard (tech_spec.md 2.4.4) ---- */
#define OTA_GUARD_COMMITTED      0
#define OTA_GUARD_STALE          1
#define OTA_GUARD_OTA_ACTIVE     2
#define OTA_GUARD_PENDING_VERIFY 3
#define OTA_GUARD_CANCELLED      4

typedef struct {
    esp_err_t (*exec)(ble_host_exec_fn_t fn, uintptr_t arg, uint32_t timeout_ms, int32_t *out_result);
    bool (*gen_current_secure)(uint32_t gen); /* gen is the current generation of a secure link */
    bool (*proto_idle)(void);                 /* protocol state is IDLE (takes the proto mutex) */
    bool (*proto_drain_stuck)(void);          /* ble_ota_proto_is_drain_stuck() */
    bool (*pending_verify)(void);             /* running image is PENDING_VERIFY */
} ble_ota_guard_ops_t;

void ble_ota_gate_set_guard_ops(const ble_ota_guard_ops_t *ops);

/* Unit-test support only: both guard slots back to the boot value (no request open). */
void ble_ota_gate_reset_guard(void);

/* Host-task exec function of the guard; public so the unit tests can run it early,
 * late or never. arg = (ticket << 1) | idx. */
int32_t ble_ota_gate_guard_fn(uintptr_t arg);

/* Caller side. See ble_ota.h ble_ota_reboot_guard_commit() for the contract. */
esp_err_t ble_ota_gate_guard_commit(uint32_t ble_gen, uint32_t timeout_ms, bool *out_override);

/* true while any guard slot is COMMITTED (either encoding). */
bool ble_ota_gate_reboot_pending(void);

#ifdef __cplusplus
}
#endif
