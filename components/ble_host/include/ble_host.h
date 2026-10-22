/**
 * @file ble_host.h
 * @brief Public interface of the ble_host component (main-facing: init/start/state)
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
 * ble_host owns NimBLE for the whole boot (NETSHELL32-5, tech_spec.md
 * section 2.2.1). Only <stdbool.h> and esp_err.h types appear here. The
 * client-facing API (registration, exec, notify, link info) is in
 * ble_host_client.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero the client table and create static sync objects and timers. Does not
 * touch the radio. Must be called before any client registers. */
esp_err_t ble_host_init(void);

/* Boot-time bring-up of controller + host + single GATT table + advertising.
 * ESP_ERR_INVALID_STATE if not initialised or already started. On failure the
 * partial bring-up is unwound (the only path that may call nimble_port_deinit)
 * and the host stays down for the rest of the boot (REQ-FUNC-73, REQ-INT-10). */
esp_err_t ble_host_start(void);

/* true once the host has synced and advertising was attempted. */
bool ble_host_is_up(void);

/* true between ble_host_start() returning OK and the first sync (or a
 * re-sync after a host reset). */
bool ble_host_is_starting(void);

#ifdef __cplusplus
}
#endif
