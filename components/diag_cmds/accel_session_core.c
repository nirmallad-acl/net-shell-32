/**
 * @file accel_session_core.c
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
 *
 * REQ-FUNC-47, 49; tech_spec.md 2.6.5.
 */
#include "accel_session_core.h"

#include <stddef.h>

void accel_core_init(accel_session_core_t *c)
{
    if (c == NULL) {
        return;
    }
    atomic_store(&c->claim, 0u);
    atomic_store(&c->active_sid, 0u);
    atomic_store(&c->cancel_sid, 0u);
    atomic_store(&c->cancel_reason, 0u);
    atomic_store(&c->owner_transport, 0u);
    atomic_store(&c->owner_gen, 0u);
    c->sid_counter = 0;
}

bool accel_core_try_claim(accel_session_core_t *c)
{
    if (c == NULL) {
        return false;
    }
    uint32_t expected = 0u;
    return atomic_compare_exchange_strong(&c->claim, &expected, 1u);
}

void accel_core_set_owner(accel_session_core_t *c, diag_transport_t t, uint32_t gen)
{
    if (c == NULL) {
        return;
    }
    atomic_store(&c->owner_transport, (uint32_t)t);
    atomic_store(&c->owner_gen, gen);
}

uint32_t accel_core_publish(accel_session_core_t *c)
{
    if (c == NULL) {
        return 0;
    }
    c->sid_counter++;
    if (c->sid_counter == 0u) {
        c->sid_counter = 1u;
    }
    atomic_store_explicit(&c->active_sid, c->sid_counter, memory_order_release);
    return c->sid_counter;
}

void accel_core_abort_unpublished(accel_session_core_t *c)
{
    if (c != NULL) {
        atomic_store_explicit(&c->claim, 0u, memory_order_release);
    }
}

bool accel_core_cancel(accel_session_core_t *c, diag_transport_t t, uint32_t gen, diag_cancel_reason_t why)
{
    if (c == NULL) {
        return false;
    }
    uint32_t sid = atomic_load_explicit(&c->active_sid, memory_order_acquire);
    if (sid == 0u) {
        return false;
    }
    uint32_t ot = atomic_load(&c->owner_transport);
    uint32_t og = atomic_load(&c->owner_gen);
    bool match = (t == DIAG_TRANSPORT_ANY) ||
                 ((uint32_t)t == ot && (t != DIAG_TRANSPORT_BLE || og == gen));
    if (!match) {
        return false;
    }
    atomic_store(&c->cancel_reason, (uint32_t)why);
    atomic_store_explicit(&c->cancel_sid, sid, memory_order_release);
    return true;
}

bool accel_core_cancelled(accel_session_core_t *c, uint32_t sid, uint32_t *why)
{
    if (c == NULL || sid == 0u) {
        return false;
    }
    if (atomic_load_explicit(&c->cancel_sid, memory_order_acquire) == sid) {
        if (why != NULL) {
            *why = atomic_load(&c->cancel_reason);
        }
        return true;
    }
    return false;
}

uint32_t accel_core_active_sid(accel_session_core_t *c)
{
    return (c != NULL) ? atomic_load_explicit(&c->active_sid, memory_order_acquire) : 0u;
}

void accel_core_release(accel_session_core_t *c)
{
    if (c == NULL) {
        return;
    }
    atomic_store_explicit(&c->active_sid, 0u, memory_order_release);
    atomic_store_explicit(&c->claim, 0u, memory_order_release);
}
