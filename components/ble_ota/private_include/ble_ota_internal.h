/**
 * @file ble_ota_internal.h
 * @brief Component-wide log tags, error codes and shared limits
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
 * Not reachable from main/ (private include directory only). Included by
 * every layer of this component -- kept free of any BLE-stack or
 * esp_ota_* header so it cannot itself become a layering violation.
 */
#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Log tags (FR-1004): one per layer, independently tunable via
 * esp_log_level_set(), matching main.c's existing per-tag convention.
 * ------------------------------------------------------------------- */
#define BLE_OTA_TAG_FACADE "ble_ota"      /* ble_ota.c, cmd_fwupgrade.c */
#define BLE_OTA_TAG_BLE    "ble_ota_ble"  /* L1: ble_ota_transport_nimble.c */
#define BLE_OTA_TAG_PROTO  "ble_ota_proto" /* L2: ble_ota_proto.c */
#define BLE_OTA_TAG_FLASH  "ble_ota_flash" /* L3: ble_ota_flash.c */
#define BLE_OTA_TAG_HEALTH "ble_ota_hlth"  /* ble_ota_health.c */

/* ---------------------------------------------------------------------
 * Component-specific esp_err_t codes (NFR-103): distinct codes for
 * invalid-argument-like, invalid-state-like, timeout-like and
 * resource-exhaustion-like conditions specific to this feature. Base
 * chosen outside every ESP-IDF-reserved error-code base in current use
 * (esp_err.h documents 0x3000..0xd000 as taken by various components).
 * ------------------------------------------------------------------- */
#define ESP_ERR_BLE_OTA_BASE            0xB000

#define ESP_ERR_BLE_OTA_PROTO_SEQUENCE  (ESP_ERR_BLE_OTA_BASE + 1) /* opcode illegal in current state */
#define ESP_ERR_BLE_OTA_PROTO_LENGTH    (ESP_ERR_BLE_OTA_BASE + 2) /* frame length/field out of range */
#define ESP_ERR_BLE_OTA_CHUNK_GAP       (ESP_ERR_BLE_OTA_BASE + 3) /* coverage-bitmap gap at 0xFC */
#define ESP_ERR_BLE_OTA_CHUNK_TOO_BIG   (ESP_ERR_BLE_OTA_BASE + 4) /* chunkSize > MAX_CHUNK_SIZE (FR-411) */
#define ESP_ERR_BLE_OTA_NOT_BONDED      (ESP_ERR_BLE_OTA_BASE + 5) /* link not encrypted+bonded (FR-301) */
#define ESP_ERR_BLE_OTA_SLOT_TOO_SMALL  (ESP_ERR_BLE_OTA_BASE + 6) /* declared size > slot (FR-406) */
#define ESP_ERR_BLE_OTA_PENDING_VERIFY  (ESP_ERR_BLE_OTA_BASE + 7) /* running image PENDING_VERIFY, Finding F-2 */
#define ESP_ERR_BLE_OTA_BUSY_DRAINING   (ESP_ERR_BLE_OTA_BASE + 8) /* ABORTING_DRAIN, §5.9 */
#define ESP_ERR_BLE_OTA_DRAIN_STUCK     (ESP_ERR_BLE_OTA_BASE + 9) /* drain watchdog expired, §5.9.6 */
#define ESP_ERR_BLE_OTA_ALREADY_RUNNING (ESP_ERR_BLE_OTA_BASE + 10) /* fwupgrade start while already up */
#define ESP_ERR_BLE_OTA_NOT_RUNNING     (ESP_ERR_BLE_OTA_BASE + 11) /* fwupgrade stop while not up */

/* ---------------------------------------------------------------------
 * Convenience aliases for the component Kconfig values, so the rest of
 * the code reads against a stable name rather than CONFIG_* directly.
 * ------------------------------------------------------------------- */
#define BLE_OTA_MAX_CHUNK_SIZE  ((size_t)CONFIG_BLE_OTA_MAX_CHUNK_SIZE)
#define BLE_OTA_MAX_PART_SIZE   ((size_t)CONFIG_BLE_OTA_MAX_PART_SIZE)

/* Defined in cmd_fwupgrade.c, called once by ble_ota_register_all()
 * (ble_ota.c). */
esp_err_t ble_ota_cmd_fwupgrade_register(void);

#ifdef __cplusplus
}
#endif
