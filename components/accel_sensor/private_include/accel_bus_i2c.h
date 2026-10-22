/**
 * @file accel_bus_i2c.h
 * @brief Private: ESP-IDF i2c_master glue implementing accel_bus_ops_t
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
#include "accel_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the I2C master bus (internal pull-ups on) and the two device handles
 * (0x53, 0x1D). On any failure everything already created is released and the
 * error is returned. */
esp_err_t accel_bus_i2c_create(const accel_bus_ops_t **ops, void **bus);

/* Releases the device handles and the bus created by accel_bus_i2c_create() (used when a
 * later init step fails). Safe to call when nothing was created. */
void accel_bus_i2c_destroy(void);

#ifdef __cplusplus
}
#endif
