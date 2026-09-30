/**
 * @file ble_ota_health.h
 * @brief Post-boot health confirmation and rollback decision interface
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
 * Independent of the L1/L2/L3 BLE data-path stack (tech_spec.md §7):
 * evaluated on every boot (FR-809), whether or not "fwupgrade start" is
 * ever issued.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The five health checks (tech_spec.md §7.2), each tagged with its
 * recoverability class (Phase 2.5 finding 1, §7.2/§7.3.1). */
typedef enum {
    BLE_OTA_HEALTH_H1_CONSOLE = 0, /* class R -- console/REPL up */
    BLE_OTA_HEALTH_H2_NVS,         /* class D -- NVS available and writable */
    BLE_OTA_HEALTH_H3_WIFI,        /* class D -- Wi-Fi STA initialised */
    BLE_OTA_HEALTH_H4_BLE_OTA,     /* class R -- ble_ota module initialised */
    BLE_OTA_HEALTH_H5_HEAP,        /* class D -- free internal heap floor */
    BLE_OTA_HEALTH_CHECK_COUNT,
} ble_ota_health_check_id_t;

typedef enum {
    BLE_OTA_HEALTH_CLASS_D = 0, /* degraded but reachable */
    BLE_OTA_HEALTH_CLASS_R,     /* recoverability-fatal (RB-INV) */
} ble_ota_health_class_t;

ble_ota_health_class_t ble_ota_health_check_class(ble_ota_health_check_id_t id);
const char             *ble_ota_health_check_name(ble_ota_health_check_id_t id);

/* One-time init, called from ble_ota_init(): reads the running
 * partition's OTA state (FR-809). If it is ESP_OTA_IMG_PENDING_VERIFY,
 * arms the periodic health-evaluation timer (§7.3); otherwise logs
 * FWUPG_BOOT and returns without arming anything -- a normal boot is
 * never misclassified as a failed update. */
esp_err_t ble_ota_health_init(void);

/* H-1/H-3 inputs, recorded once by main.c (see ble_ota.h's
 * ble_ota_note_boot_status() -- this is its implementation). */
void ble_ota_health_record_boot_status(esp_err_t wifi_init,
                                        esp_err_t wifi_set_mode,
                                        esp_err_t wifi_start,
                                        esp_err_t console_new_repl,
                                        esp_err_t console_start_repl);

/* H-4 input: recorded by the ble_ota facade after ble_ota_init() and
 * ble_ota_register_all() both complete. */
void ble_ota_health_record_ble_ota_status(esp_err_t init_result, esp_err_t register_result);

/* True while the running image is still PENDING_VERIFY (Finding F-2,
 * §6.4). Used by ble_ota_transport_start() to refuse "fwupgrade start"
 * without raising the radio. */
bool ble_ota_health_is_pending_verify(void);

/* Upper-bound seconds remaining until the health deadline, floored at 0,
 * for §6.4(1)'s synchronous console text and "fwupgrade status". */
uint32_t ble_ota_health_remaining_deadline_s(void);

#ifdef __cplusplus
}
#endif
