/**
 * @file ble_ota.h
 * @brief Public interface for the ble_ota component
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-09-09
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
 * This is the entire public surface: three functions. No NimBLE,
 * esp_ota_*, Wi-Fi or FreeRTOS types leak here -- everything a
 * command/layer implementation needs beyond this lives in
 * private_include/ (NFR-402). All dependencies of this component are
 * therefore PRIV_REQUIRES in CMakeLists.txt, exactly as net_cli's are.
 *
 * ble_ota_note_boot_status() is a necessary, minimal addition beyond the
 * two calls tech_spec.md §3.6 describes ("main.c gains exactly two
 * calls"). Health checks H-1 (console/REPL up) and H-3 (Wi-Fi STA
 * initialised, tech_spec.md §7.2) need main.c's own boot-step results;
 * NFR-303 forbids modifying net_cli to add a getter for its private boot
 * status, and net_cli.h's existing public surface exposes no such
 * accessor, so this is the only source for those two facts. It is called
 * once, after esp_console_start_repl() -- the last boot step whose
 * result either check needs -- alongside the two calls already
 * documented below.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create this component's synchronization primitives (state mutex,
 * buffer-release semaphore, event queues) and evaluate the post-boot
 * rollback/health state (FR-809): if the running image is
 * ESP_OTA_IMG_PENDING_VERIFY, arms the health-confirmation timer
 * (tech_spec.md §7.3); otherwise logs FWUPG_BOOT and does nothing else.
 *
 * Does NOT touch the BLE controller or host (FR-101) -- BLE bring-up
 * happens only on "fwupgrade start". Idempotent: a second call returns
 * ESP_ERR_INVALID_STATE and changes nothing.
 *
 * MUST be called once during boot, before ble_ota_register_all(). A
 * failure is logged; the caller (main.c) MUST NOT treat it as fatal to
 * the rest of the boot sequence (FR-904's degraded-boot philosophy). */
esp_err_t ble_ota_init(void);

/* Register "fwupgrade start|stop|status" on the existing shared
 * esp_console REPL (FR-901/902), mirroring net_cli_register_all().
 * MUST be called after esp_console_new_repl_uart() and before
 * esp_console_start_repl(). A failure here MUST NOT prevent the REPL
 * from starting or the other commands from registering (FR-904). */
esp_err_t ble_ota_register_all(void);

/* Records the results of main.c's own Wi-Fi and console boot steps, for
 * health checks H-1 and H-3 (tech_spec.md §7.2). MUST be called once,
 * after esp_console_start_repl() returns, regardless of whether earlier
 * steps were skipped (NET_CLI_ERR_SKIPPED, i.e. ESP_ERR_INVALID_STATE, is
 * a valid value to pass here and is treated as "not OK", not as a
 * crash). Safe to call even if ble_ota_init() itself failed. */
void ble_ota_note_boot_status(esp_err_t wifi_init,
                               esp_err_t wifi_set_mode,
                               esp_err_t wifi_start,
                               esp_err_t console_new_repl,
                               esp_err_t console_start_repl);

#ifdef __cplusplus
}
#endif
