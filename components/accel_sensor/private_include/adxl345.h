/**
 * @file adxl345.h
 * @brief PURE: ADXL345 identity check, configuration steps, burst read and decode
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

#include <stdint.h>
#include "esp_err.h"
#include "accel_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration is split into single-transaction steps so the caller can take
 * the sensor lock once per transaction (REQ-PERF-05). */
#define ADXL345_CFG_STEPS 8u

typedef struct {
    const accel_bus_ops_t *ops;
    void                  *bus;
    uint8_t                addr;
    uint32_t               timeout_ms;
} adxl345_t;

/* Reads DEVID; ESP_ERR_ACCEL_IDENTITY unless it equals 0xE5. No write is ever
 * issued by this call (REQ-FUNC-50). */
esp_err_t adxl345_check_identity(const adxl345_t *d);

/* One configuration transaction: steps 0..5 write POWER_CTL=standby, BW_RATE,
 * DATA_FORMAT, FIFO_CTL, INT_ENABLE, POWER_CTL=measure; steps 6 and 7 read back
 * BW_RATE and DATA_FORMAT (ESP_ERR_ACCEL_CONFIG on mismatch). Bus errors are
 * propagated unchanged. ESP_ERR_INVALID_ARG for step >= ADXL345_CFG_STEPS. */
esp_err_t adxl345_config_step(const adxl345_t *d, unsigned step);

/* One 13-byte burst read of 0x2C..0x38: coherent X/Y/Z from one conversion AND the
 * BW_RATE, POWER_CTL (measure set, sleep clear), DATA_FORMAT and FIFO_CTL (bypass) registers,
 * which must still hold the configured values; otherwise ESP_ERR_ACCEL_CONFIG and xyz is
 * not written. */
esp_err_t adxl345_read_raw(const adxl345_t *d, int16_t xyz[3]);

/* Decodes the 6 burst bytes (sign preserved). */
void  adxl345_decode(const uint8_t raw[6], int16_t xyz[3]);
float adxl345_raw_to_g(int16_t raw);

#ifdef __cplusplus
}
#endif
