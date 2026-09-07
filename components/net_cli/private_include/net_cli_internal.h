/**
 * @file net_cli_internal.h
 * @brief Component-internal interface: state, sync primitives, and command registration
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-09-07
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
 * Not reachable from main/ (private include directory only). Declares sync
 * primitives, boot-status accessors, shared constants, format helpers, and
 * per-command registrars that net_cli.c aggregates.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared compile-time limits and output formats. */

#define NET_CLI_SCAN_MAX_APS       20u     /* Maximum APs to cache in scan results. */
#define NET_CLI_WIFI_CONNECT_MS    10000u  /* Timeout for connection attempts. */
#define NET_CLI_PING_WAIT_MS       8000u   /* Timeout for ping to complete. */
#define NET_CLI_PING_JOIN_MS       3000u   /* Timeout for ping task cleanup on abort. */
#define NET_CLI_PING_TASK_STACK    4096u   /* Stack size for ping task (overrides default). */
#define NET_CLI_PING_COUNT         4u      /* Number of ICMP packets to send (overrides default). */

/* Lowercase colon-separated MAC format. */
#define NET_CLI_MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

/* wifi_connect result: outcome code and failure reason if applicable. */

typedef enum {
    NET_CLI_CONNECT_CONNECTED = 0,
    NET_CLI_CONNECT_FAILED,
    NET_CLI_CONNECT_TIMEOUT,
} net_cli_connect_outcome_t;

typedef struct {
    net_cli_connect_outcome_t outcome;
    uint8_t                   reason;  /* Disconnect reason; valid iff outcome == FAILED. */
} net_cli_connect_result_t;

/* ping session state shared with esp_ping callbacks. One static instance
 * suffices because the reclaim gate ensures at most one session exists
 * concurrently. Passed as esp_ping_callbacks_t.cb_args. */
typedef struct {
    volatile bool abandoned;  /* If true, callbacks must not print. */
} net_cli_ping_state_t;

/* Initialize the event group, ping semaphore, and Wi-Fi/IP event handlers.
 * Called once from net_cli_init(). Idempotent: returns ESP_ERR_INVALID_STATE
 * on a second call. */
esp_err_t net_cli_wifi_state_init(void);

/* True iff Wi-Fi subsystem boot succeeded. Gate for commands that require
 * Wi-Fi (wifiscan, wifi_connect, ping). */
bool net_cli_wifi_ready(void);

/* True iff sync primitives are ready to use. Must be true before any
 * blocking wait on the event group or ping semaphore. */
bool net_cli_sync_ready(void);

/* Read-only accessor for the boot-status record. Used by boot banner. */
const net_cli_boot_status_t *net_cli_boot_status_get(void);

/* Run a full Wi-Fi connect sequence: disconnect, settle, arm event handlers,
 * configure and connect, wait for result. Returns ESP_ERR_INVALID_STATE if
 * sync primitives unavailable, ESP_ERR_INVALID_ARG if args are NULL,
 * otherwise ESP_OK with *out populated. */
esp_err_t net_cli_wifi_connect_attempt(const wifi_config_t *cfg,
                                        uint32_t timeout_ms,
                                        net_cli_connect_result_t *out);

/* Accessors for the ping semaphore and session state record. Used by
 * cmd_ping.c. Callers must gate on net_cli_sync_ready() before blocking. */
SemaphoreHandle_t     net_cli_ping_done_sem(void);
net_cli_ping_state_t *net_cli_ping_state(void);

/* ---- per-command registrars (net_cli.c aggregates these) ---- */

esp_err_t net_cli_register_ifconfig(void);
esp_err_t net_cli_register_wifiscan(void);
esp_err_t net_cli_register_wifi_connect(void);
esp_err_t net_cli_register_ping(void);

#ifdef __cplusplus
}
#endif
