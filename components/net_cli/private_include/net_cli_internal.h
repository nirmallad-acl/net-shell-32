/*
 * net_cli_internal.h -- component-internal contract.
 *
 * NOT reachable from main/ (this directory is on net_cli's
 * PRIV_INCLUDE_DIRS only). Declares the sync primitives, the boot-status
 * accessor, the shared compile-time limits and output format helpers, and
 * the four per-command registrars that net_cli.c aggregates. See
 * tech_spec.md §5.4.
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

/* ---- Shared compile-time limits and formats (§7, §8.5) ---- */

#define NET_CLI_SCAN_MAX_APS       20u     /* GATE 1 / OQ-4 */
#define NET_CLI_WIFI_CONNECT_MS    10000u  /* OQ-5 */
#define NET_CLI_PING_WAIT_MS       8000u   /* §6.5.3 -- GATE 2 R-6: accepted as-is, do not shorten */
#define NET_CLI_PING_JOIN_MS       3000u   /* §6.5.3 -- derived from esp_ping's ~2 s worst-case residual + margin */
#define NET_CLI_PING_TASK_STACK    4096u   /* §8.5 / DA-6 -- MUST override ESP_TASK_PING_STACK */
#define NET_CLI_PING_COUNT         4u      /* FR-44 -- MUST override esp_ping's default of 5 */

/* Lowercase colon-separated MAC format. MACSTR/MAC2STR are not public
 * headers for a Wi-Fi-only build in v6.0.2 -- do not use them (§7.1/§8.1). */
#define NET_CLI_MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

/* ---- wifi_connect outcome (§6.4, §7.3.4) ---- */

typedef enum {
    NET_CLI_CONNECT_CONNECTED = 0,
    NET_CLI_CONNECT_FAILED,
    NET_CLI_CONNECT_TIMEOUT,
} net_cli_connect_outcome_t;

typedef struct {
    net_cli_connect_outcome_t outcome;
    uint8_t                   reason;  /* wifi_err_reason_t value; valid iff outcome == FAILED */
} net_cli_connect_result_t;

/* ---- ping session state shared with the esp_ping callbacks (§6.5.2) ----
 * Passed as esp_ping_callbacks_t.cb_args. One static instance is sufficient
 * because the reclaim gate (§6.5.4) guarantees at most one session exists
 * at any time. */
typedef struct {
    volatile bool abandoned;  /* REPL stopped waiting: callbacks must not print */
} net_cli_ping_state_t;

/* ---- net_cli_wifi_state.c: sole owner of the event group, ping semaphore,
 * last-disconnect-reason and boot-status record (P-B4) ---- */

/* Creates the event group + ping semaphore and registers the WIFI_EVENT/
 * IP_EVENT handler. Called once, from net_cli_init(). Idempotent: a second
 * call returns ESP_ERR_INVALID_STATE. */
esp_err_t net_cli_wifi_state_init(void);

/* True iff wifi_init, wifi_set_mode, wifi_start AND cli_init (DA-2) all
 * recorded ESP_OK in the boot-status record. Gate used by wifiscan,
 * wifi_connect and ping (§7.0 rule 5). */
bool net_cli_wifi_ready(void);

/* True iff net_cli_init() itself recorded ESP_OK and both the event group
 * and the ping semaphore are non-NULL. The last-line-of-defence predicate
 * that must be true before ANY blocking wait on those handles -- a NULL
 * FreeRTOS handle must never reach xEventGroupWaitBits/xSemaphoreTake even
 * if the boot-status bookkeeping somehow disagrees (DA-2). */
bool net_cli_sync_ready(void);

/* Read-only accessor for the boot-status record net_cli_set_boot_status()
 * copied in (used by net_cli_print_boot_banner()). */
const net_cli_boot_status_t *net_cli_boot_status_get(void);

/* Runs the full §6.4 connect sequence: pre-emptive disconnect, settle,
 * arm, esp_wifi_set_config()+esp_wifi_connect(), bounded wait, classify.
 * Returns ESP_ERR_INVALID_STATE without blocking if net_cli_sync_ready()
 * is false; ESP_ERR_INVALID_ARG if cfg/out is NULL; otherwise ESP_OK with
 * *out populated (including on a Wi-Fi driver call failure that occurs
 * mid-sequence -- see cmd_wifi_connect.c for exactly which esp_err_t
 * values are surfaced instead). */
esp_err_t net_cli_wifi_connect_attempt(const wifi_config_t *cfg,
                                        uint32_t timeout_ms,
                                        net_cli_connect_result_t *out);

/* Accessors for the boot-created ping semaphore and the single static ping
 * session state record (§6.5.2). Both return NULL/never-NULL consistently
 * with net_cli_wifi_state_init() having run; callers must still gate on
 * net_cli_sync_ready() before blocking on the semaphore (DA-2). */
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
