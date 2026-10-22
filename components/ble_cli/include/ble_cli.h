/**
 * @file ble_cli.h
 * @brief Public interface of ble_cli: the BLE UART-style CLI client of ble_host
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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the CLI service with ble_host (must run after ble_host_init() and
 * before ble_host_start()) and creates the worker task and the static queues.
 * A failure is logged by the caller; the serial commands are unaffected. */
esp_err_t ble_cli_init(void);

#ifdef __cplusplus
}
#endif
