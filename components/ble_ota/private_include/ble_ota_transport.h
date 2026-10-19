/**
 * @file ble_ota_transport.h
 * @brief L1 interface: NimBLE transport bring-up/teardown and status, called by the CLI
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
 * This is the ONLY header cmd_fwupgrade.c needs for the BLE lifecycle
 * itself; ble_ota_transport_nimble.c is the ONLY translation unit in the
 * whole component that includes a NimBLE header (NFR-403, tech_spec.md
 * §3.3). No NimBLE type appears in this header's signatures.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool advertising;
    bool connected;
    bool bonded;
    char peer_addr_str[18]; /* "xx:xx:xx:xx:xx:xx", or "" if not connected */
} ble_ota_transport_status_t;

/* FR-102: checked, ordered bring-up (controller -> host -> GATT service
 * registration -> advertising), aborting on the first failure and
 * unwinding any partially-initialised state so a later call can succeed.
 * Refuses with ESP_ERR_BLE_OTA_ALREADY_RUNNING if already up, and with
 * ESP_ERR_BLE_OTA_PENDING_VERIFY (Finding F-2, tech_spec.md §6.4(1))
 * without touching the radio at all if the running image is still
 * PENDING_VERIFY. */
esp_err_t ble_ota_transport_start(void);

/* FR-103: stops advertising, terminates any active connection,
 * deregisters the GATT service, disables host and controller, releases
 * their heap. Safe to call when not running (ESP_ERR_BLE_OTA_NOT_RUNNING,
 * not escalated to an error the caller must handle specially). */
esp_err_t ble_ota_transport_stop(void);

bool ble_ota_transport_is_running(void);

/* FR-907: BLE/advertising/connection/bonding state for "fwupgrade
 * status". NULL is rejected with ESP_ERR_INVALID_ARG (NFR-104). */
esp_err_t ble_ota_transport_get_status(ble_ota_transport_status_t *out);

#ifdef __cplusplus
}
#endif
