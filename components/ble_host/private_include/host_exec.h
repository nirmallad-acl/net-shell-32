/**
 * @file host_exec.h
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
 */

/*
 * The request record and the caller/executor protocol of tech_spec.md
 * section 2.3.5, with time, lock, semaphore and event-queue access injected
 * through host_exec_ops_t so the whole state machine is unit-testable on a
 * PC (test_host_exec). ble_host.c supplies the FreeRTOS/NimBLE ops.
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

#define HOST_EXEC_T_FN_MAX_MS    250u /* longest an exec fn may run once started */
#define HOST_EXEC_T_COMPLETE_MS  350u /* T_FN_MAX + 100 ms completion window */

typedef enum {
    HOST_EXEC_IDLE = 0,
    HOST_EXEC_PENDING,
    HOST_EXEC_RUNNING,
    HOST_EXEC_DONE,
    HOST_EXEC_ABANDONED,
} host_exec_state_t;

typedef struct {
    uint32_t           seq;
    ble_host_exec_fn_t fn;
    uintptr_t          arg;
    int32_t            result;
    _Atomic uint32_t   done_seq;
    _Atomic uint32_t   state; /* host_exec_state_t */
} host_exec_t;

typedef struct {
    void *ctx;
    int64_t (*now_us)(void *ctx);
    bool    (*mtx_take)(void *ctx, uint32_t timeout_ms);
    void    (*mtx_give)(void *ctx);
    bool    (*sem_take)(void *ctx, uint32_t timeout_ms);
    void    (*sem_give)(void *ctx);
    bool    (*post)(void *ctx);          /* queue the executor event on the host eventq */
    bool    (*host_up)(void *ctx);
    bool    (*on_host_task)(void *ctx);
    void    (*log_stuck)(void *ctx, uint32_t seq);
} host_exec_ops_t;

void host_exec_init(host_exec_t *x);

/* Caller side. See ble_host_client.h for the return-code contract. */
esp_err_t host_exec_call(host_exec_t *x, const host_exec_ops_t *ops, ble_host_exec_fn_t fn,
                         uintptr_t arg, uint32_t timeout_ms, int32_t *out_result);

/* Executor side: the handler of the single static exec event, host task only. */
void host_exec_run(host_exec_t *x, const host_exec_ops_t *ops);

#ifdef __cplusplus
}
#endif
