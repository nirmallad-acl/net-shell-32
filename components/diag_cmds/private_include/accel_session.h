/**
 * @file accel_session.h
 * @brief Private: the system-wide accel_xyz session engine (lock-free claim and cancel)
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
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "diag_cmds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the session task (stack on the heap, once) and the serial done semaphore. */
esp_err_t accel_session_init(void);

/* Claims the single system-wide session (REQ-FUNC-47), writes the start line
 * through ctx and wakes the session task.
 * ESP_ERR_INVALID_STATE: a session is already running (nothing was disturbed).
 * ESP_FAIL: the start line could not be written (the claim was released).
 * ESP_ERR_INVALID_ARG / ESP_ERR_NOT_SUPPORTED: bad ctx / engine not initialised. */
esp_err_t accel_session_start(const diag_ctx_t *ctx);

/* Serial owner only: waits for the session to finish. true if it did. */
bool accel_session_wait_done(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
