/**
 * @file accel_session_core.h
 * @brief PURE: lock-free claim, publish and cancel state of the system-wide accel session
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
 * The atomics of tech_spec.md 2.6.5, extracted from accel_session.c without changing
 * behaviour so they are unit-tested (test_session). No mutex: ownership is one CAS on
 * `claim`, publication is `active_sid` (written last), and a cancel names the session id
 * it targets so a late cancel can never hit a newer session. Task wake-ups are done by the
 * caller (accel_session.c) after core_cancel() returns true.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "diag_cmds.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    _Atomic uint32_t claim;           /* 0 = free; the start CAS 0 -> 1 is the single ownership point */
    _Atomic uint32_t active_sid;      /* published last at start, cleared first at release; never 0 when active */
    _Atomic uint32_t cancel_sid;
    _Atomic uint32_t cancel_reason;
    _Atomic uint32_t owner_transport; /* read by the cancel path from other tasks */
    _Atomic uint32_t owner_gen;
    uint32_t         sid_counter;     /* touched only by the claimant */
} accel_session_core_t;

void     accel_core_init(accel_session_core_t *c);
/* CAS claim 0 -> 1. false if a session is already running (it is not disturbed). */
bool     accel_core_try_claim(accel_session_core_t *c);
/* Claimant only, before publication. */
void     accel_core_set_owner(accel_session_core_t *c, diag_transport_t t, uint32_t gen);
/* Claimant only: allocates the next session id (monotonic, never 0), publishes it last
 * with a release store and returns it. */
uint32_t accel_core_publish(accel_session_core_t *c);
/* Claimant only: gives up a claim that was never published. */
void     accel_core_abort_unpublished(accel_session_core_t *c);
/* Any task, lock-free. Records a cancel only if a session is active and it belongs to
 * transport t (and, for BLE, to generation gen; t == ANY matches every session).
 * Returns true if a cancel was recorded (the caller then wakes the session task). */
bool     accel_core_cancel(accel_session_core_t *c, diag_transport_t t, uint32_t gen, diag_cancel_reason_t why);
/* Session task: has the session with this id been cancelled? Stores the reason. */
bool     accel_core_cancelled(accel_session_core_t *c, uint32_t sid, uint32_t *why);
uint32_t accel_core_active_sid(accel_session_core_t *c);
/* Session task: the session id is cleared first, then the claim. */
void     accel_core_release(accel_session_core_t *c);

#ifdef __cplusplus
}
#endif
