/**
 * @file diag_cmds.h
 * @brief Public interface of diag_cmds: the shared command layer for serial and BLE
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
 * NETSHELL32-5, tech_spec.md sections 2.2.3 and 2.6. The five commands
 * (hw_version, fw_version, accel_xyz, reboot_reason, reboot) are implemented
 * once and reach both transports through diag_ctx_t. Only <stdbool.h>,
 * <stddef.h>, <stdint.h> and esp_err.h types appear here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_LINE_MAX 128 /* buffer size of one response line: 127 characters + NUL (LF added by the sink) */

typedef enum {
    DIAG_TRANSPORT_SERIAL = 0,
    DIAG_TRANSPORT_BLE,
    DIAG_TRANSPORT_ANY,
} diag_transport_t;

typedef enum {
    DIAG_CMD_HW_VERSION = 0,
    DIAG_CMD_FW_VERSION,
    DIAG_CMD_ACCEL_XYZ,
    DIAG_CMD_REBOOT_REASON,
    DIAG_CMD_REBOOT,
    DIAG_CMD_COUNT,
} diag_cmd_id_t;

typedef enum {
    DIAG_CANCEL_DISCONNECT = 0,
    DIAG_CANCEL_REBOOT,
    DIAG_CANCEL_TX_FAIL,
    DIAG_CANCEL_TIMEOUT, /* serial REPL gave up waiting for the session (additive) */
} diag_cancel_reason_t;

typedef struct diag_ctx diag_ctx_t;
struct diag_ctx {                 /* copied by value; no pointers into stacks */
    diag_transport_t transport;
    uint32_t         gen;         /* BLE connection generation; 0 for serial */
    int64_t          deadline_us; /* 0 = none; else write_line/flush stop at this esp_timer time
                                     (used by reboot, tech_spec.md 2.6.6) */
    esp_err_t (*write_line)(const diag_ctx_t *ctx, const char *line, size_t len); /* line without LF */
    esp_err_t (*flush)(const diag_ctx_t *ctx, uint32_t timeout_ms);
    bool      (*is_current)(const diag_ctx_t *ctx); /* false once the originating link/gen is gone */
    uint32_t  t_rx_ms;        /* esp_timer ms when the full line reached the CLI layer (log only,
                                 additive field for the BLECLI_CMD t_rx_ms test hook H-3) */
};

/* Creates the accel session task (heap, once) and the 30 s resource monitor.
 * A failure is logged by the caller; serial commands that do not need the
 * session still work. */
esp_err_t diag_cmds_init(void);

/* Registers the five commands and the serial-only hw_rev_set on the existing
 * esp_console REPL. No esp_console_new_repl_* call is made here. */
esp_err_t diag_cmds_register_serial(void);

/* Closed lookup of the five command names (case-sensitive, exact length). */
esp_err_t diag_cmd_lookup_ble(const char *name, size_t len, diag_cmd_id_t *out);
/* Never NULL; "?" for an invalid id. */
const char *diag_cmd_name(diag_cmd_id_t id);

/* Runs one command on the given transport and logs BLECLI_CMD. */
esp_err_t diag_cmd_run(diag_cmd_id_t id, const diag_ctx_t *ctx);

/* "ERR <code>[ cmd=<name>][ <extra>]" through ctx->write_line. code must be one
 * of the closed set (unknown_command, invalid_argument, oversized_input,
 * unavailable, sensor_error, sensor_stale, busy, internal). cmd and extra may
 * be NULL. */
esp_err_t diag_emit_error(const diag_ctx_t *ctx, const char *code, const char *cmd, const char *extra);

/* Lock-free and non-blocking (atomics + one task notification); safe on the
 * host task. Cancels the active session only if it belongs to transport t and,
 * for BLE, to generation gen (gen is ignored for SERIAL; DIAG_CANCEL_REBOOT
 * with t = DIAG_TRANSPORT_ANY cancels any session). */
void diag_session_cancel(diag_transport_t t, uint32_t gen, diag_cancel_reason_t why);

/* Resource monitor (HEAP_STAT / STACK_HWM log lines). `why` is a short ASCII tag. */
void diag_resmon_emit(const char *why);
/* Optional provider of the callback-time maxima appended to HEAP_STAT. */
void diag_resmon_set_provider(void (*fn)(uint32_t *cb_max_us, uint32_t *ota_rx_max_us));

#ifdef __cplusplus
}
#endif
