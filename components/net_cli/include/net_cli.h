/**
 * @file net_cli.h
 * @brief Public interface for the net_cli component
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
 * This is the ENTIRE public surface: four functions and one POD struct.
 * No handle types, no callbacks, no IDF types are leaked here -- anything a
 * command implementation needs beyond this lives in net_cli_internal.h
 * (private include directory). This enforces a compile-time boundary.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel recorded for a boot step that was NOT executed because a step it
 * functionally depends on had already failed (tech_spec.md §6.1.1). Distinct
 * from ESP_OK (ran, succeeded) and from any real error (ran, failed) -- the
 * boot banner and net_cli_wifi_ready() must be able to tell "skipped" from
 * "failed". */
#define NET_CLI_ERR_SKIPPED   ESP_ERR_INVALID_STATE

/* Outcome of each boot step (FR-1..FR-7) plus net_cli_init() itself, as
 * observed by app_main().
 *   ESP_OK              = the step ran and succeeded.
 *   NET_CLI_ERR_SKIPPED  = the step was deliberately not called (a
 *                          prerequisite it depends on had already failed).
 *   anything else        = the step ran and returned that error.
 * Zero-initialise and fill in as you go; see tech_spec.md §6.1.1 for the
 * binding skip rules. */
typedef struct {
    esp_err_t nvs_init;         /* FR-1  / FR-1.1 */
    esp_err_t netif_init;       /* FR-2  */
    esp_err_t event_loop;       /* FR-3  */
    esp_err_t netif_create_sta; /* FR-4  */
    esp_err_t wifi_init;        /* FR-5  */
    esp_err_t wifi_set_mode;    /* FR-6  */
    esp_err_t wifi_start;       /* FR-7  */
    esp_err_t cli_init;         /* net_cli_init() itself -- boot step 7 (DA-2) */
} net_cli_boot_status_t;

/* Create the CLI's sync primitives and register the WIFI_EVENT/IP_EVENT
 * handler. MUST be called after esp_event_loop_create_default() and
 * esp_wifi_set_mode(), and before esp_wifi_start(). Idempotent: a second
 * call returns ESP_ERR_INVALID_STATE and changes nothing.
 *
 * Its result is NOT optional bookkeeping: on failure the event group and/or
 * the event handlers do not exist, so wifi_connect's and ping's waits could
 * never be satisfied. app_main() MUST store the returned value into
 * net_cli_boot_status_t.cli_init, and MUST NOT call esp_wifi_start() with
 * the expectation that connect/ping will work if it failed. See
 * tech_spec.md §6.1.1 and DA-2. */
esp_err_t net_cli_init(void);

/* Record what did or did not come up at boot (OQ-7 / FR-11). Commands
 * consult this to decide whether they can run. NULL is rejected with
 * ESP_ERR_INVALID_ARG.
 *
 * IMPLEMENTATION CONTRACT (binding -- DA-10): this function copies *status
 * BY VALUE into a file-scope static net_cli_boot_status_t. It does NOT
 * retain the caller's pointer. app_main()'s net_cli_boot_status_t is a
 * stack local whose lifetime ends when app_main() returns after
 * esp_console_start_repl(), whereas net_cli_wifi_ready() is read from the
 * REPL task for the rest of the program's life -- so a pointer-retaining
 * implementation would be a use-after-free on reclaimed stack. The
 * parameter is const-qualified for exactly this reason; treat it as
 * read-once. */
esp_err_t net_cli_set_boot_status(const net_cli_boot_status_t *status);

/* Register ifconfig, wifiscan, wifi_connect and ping with the console.
 * MUST be called after esp_console_new_repl_uart() (which initialises the
 * console module) and before esp_console_start_repl() (FR-10).
 * Returns the first failure; registration is all-or-nothing from the
 * caller's view. */
esp_err_t net_cli_register_all(void);

/* Print the degraded-boot banner, if any (FR-11). No-op when every boot
 * step returned ESP_OK. Call after net_cli_register_all() and before
 * esp_console_start_repl(), so the banner never races the prompt. */
void net_cli_print_boot_banner(void);

#ifdef __cplusplus
}
#endif
