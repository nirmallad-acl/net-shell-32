/**
 * @file host_exec.c
 * @brief PURE: the "run on host task" request state machine behind ble_host_exec()
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
 * tech_spec.md section 2.3.5 (D-01, D-03, ADV-CONC-12, R3-02). One overall
 * deadline D covers the mutex take, the wait for fn to start (D_start =
 * D - T_COMPLETE) and the completion window. Completion is only ever
 * accepted as (state == DONE && done_seq == my_seq) after a wake.
 */
#include "host_exec.h"

#include <string.h>

static uint32_t remaining_ms(const host_exec_ops_t *ops, int64_t deadline_us)
{
    int64_t now = ops->now_us(ops->ctx);
    if (now >= deadline_us) {
        return 0;
    }
    int64_t rem_us = deadline_us - now;
    int64_t rem_ms = (rem_us + 999) / 1000;
    if (rem_ms > (int64_t)UINT32_MAX) {
        rem_ms = (int64_t)UINT32_MAX;
    }
    return (uint32_t)rem_ms;
}

static bool is_done_for(host_exec_t *x, uint32_t my_seq)
{
    return atomic_load_explicit(&x->state, memory_order_acquire) == HOST_EXEC_DONE &&
           atomic_load_explicit(&x->done_seq, memory_order_acquire) == my_seq;
}

void host_exec_init(host_exec_t *x)
{
    if (x == NULL) {
        return;
    }
    x->seq = 0;
    x->fn = NULL;
    x->arg = 0;
    x->result = 0;
    atomic_store(&x->done_seq, 0u);
    atomic_store(&x->state, (uint32_t)HOST_EXEC_IDLE);
}

/* Steps 3-6 of the caller procedure; runs under the exec mutex. */
static esp_err_t exec_locked(host_exec_t *x, const host_exec_ops_t *ops, ble_host_exec_fn_t fn,
                             uintptr_t arg, int64_t d_start_us, int64_t d_us, int32_t *out_result)
{
    /* Step 3: a leftover RUNNING request from an earlier indeterminate call. */
    while (atomic_load_explicit(&x->state, memory_order_acquire) == HOST_EXEC_RUNNING) {
        uint32_t rem = remaining_ms(ops, d_start_us);
        if (rem == 0) {
            return ESP_ERR_TIMEOUT;
        }
        (void)ops->sem_take(ops->ctx, rem);
    }

    /* Step 4: the caller owns the reset to a fresh request. */
    (void)ops->sem_take(ops->ctx, 0); /* discard any late give */
    x->seq++;
    const uint32_t my_seq = x->seq;
    x->fn = fn;
    x->arg = arg;
    atomic_store_explicit(&x->state, (uint32_t)HOST_EXEC_PENDING, memory_order_release);

    /* Step 5: wake the executor. If an abandoned request left the event
     * queued, this put is a no-op and the queued event runs this request. */
    if (!ops->post(ops->ctx)) {
        uint32_t exp = HOST_EXEC_PENDING;
        if (atomic_compare_exchange_strong(&x->state, &exp, (uint32_t)HOST_EXEC_ABANDONED)) {
            return ESP_ERR_TIMEOUT;
        }
        /* The executor already claimed it: fall through to the normal wait. */
    }

    /* Step 6: wait with re-validation until D_start. */
    for (;;) {
        uint32_t rem = remaining_ms(ops, d_start_us);
        if (rem == 0) {
            break;
        }
        (void)ops->sem_take(ops->ctx, rem);
        if (is_done_for(x, my_seq)) {
            if (out_result != NULL) {
                *out_result = x->result;
            }
            return ESP_OK;
        }
    }

    uint32_t exp = HOST_EXEC_PENDING;
    if (atomic_compare_exchange_strong(&x->state, &exp, (uint32_t)HOST_EXEC_ABANDONED)) {
        return ESP_ERR_TIMEOUT; /* fn will never run */
    }
    /* fn started (RUNNING) or already finished (DONE): keep waiting until D. */
    for (;;) {
        if (is_done_for(x, my_seq)) {
            if (out_result != NULL) {
                *out_result = x->result;
            }
            return ESP_OK;
        }
        uint32_t rem = remaining_ms(ops, d_us);
        if (rem == 0) {
            break;
        }
        (void)ops->sem_take(ops->ctx, rem);
    }
    if (ops->log_stuck != NULL) {
        ops->log_stuck(ops->ctx, my_seq);
    }
    return ESP_ERR_BLE_HOST_EXEC_INDETERMINATE;
}

esp_err_t host_exec_call(host_exec_t *x, const host_exec_ops_t *ops, ble_host_exec_fn_t fn,
                         uintptr_t arg, uint32_t timeout_ms, int32_t *out_result)
{
    if (x == NULL || ops == NULL || fn == NULL || timeout_ms <= HOST_EXEC_T_COMPLETE_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Step 1: reject without any lock. */
    if (ops->on_host_task(ops->ctx)) {
        return ESP_ERR_BLE_HOST_WRONG_TASK;
    }
    if (!ops->host_up(ops->ctx)) {
        return ESP_ERR_BLE_HOST_NOT_UP;
    }

    const int64_t t0 = ops->now_us(ops->ctx);
    const int64_t d_us = t0 + (int64_t)timeout_ms * 1000;
    const int64_t d_start_us = d_us - (int64_t)HOST_EXEC_T_COMPLETE_MS * 1000;

    /* Step 2: the mutex wait is part of the same deadline. */
    uint32_t wait = remaining_ms(ops, d_start_us);
    if (!ops->mtx_take(ops->ctx, wait)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t rc = exec_locked(x, ops, fn, arg, d_start_us, d_us, out_result);
    ops->mtx_give(ops->ctx);
    return rc;
}

void host_exec_run(host_exec_t *x, const host_exec_ops_t *ops)
{
    if (x == NULL || ops == NULL) {
        return;
    }
    uint32_t exp = HOST_EXEC_PENDING;
    if (!atomic_compare_exchange_strong_explicit(&x->state, &exp, (uint32_t)HOST_EXEC_RUNNING,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return; /* ABANDONED or IDLE: the abandoned fn must never run */
    }
    /* Read seq/fn/arg only after the claiming CAS: the caller wrote them before
     * its release store of PENDING and cannot write them while RUNNING. */
    const uint32_t s = x->seq;
    ble_host_exec_fn_t fn = x->fn;
    const uintptr_t arg = x->arg;
    int32_t r = 0;
    if (fn != NULL) {
        r = fn(arg);
    }
    x->result = r;
    atomic_store_explicit(&x->done_seq, s, memory_order_release);
    atomic_store_explicit(&x->state, (uint32_t)HOST_EXEC_DONE, memory_order_release);
    ops->sem_give(ops->ctx);
}
