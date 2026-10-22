/**
 * @file accel_bus.h
 * @brief Private: bus operations the ADXL345 driver and the selector are written against
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
 * PURE: no IDF header besides esp_err.h and the public accel_sensor.h error
 * codes. The I2C glue (accel_bus_i2c.c) and the host test mock both implement
 * this table. Every operation returns ESP_OK or one of ESP_ERR_ACCEL_NOT_PRESENT,
 * ESP_ERR_ACCEL_TIMEOUT, ESP_ERR_ACCEL_BUS.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "accel_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*probe)(void *bus, uint8_t addr, uint32_t timeout_ms);
    esp_err_t (*write_reg)(void *bus, uint8_t addr, uint8_t reg, uint8_t val, uint32_t timeout_ms);
    esp_err_t (*read_regs)(void *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n, uint32_t timeout_ms);
    esp_err_t (*recover)(void *bus); /* i2c_master_bus_reset */
} accel_bus_ops_t;

#ifdef __cplusplus
}
#endif
