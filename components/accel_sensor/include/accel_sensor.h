/**
 * @file accel_sensor.h
 * @brief Public interface of accel_sensor: one sample API over the ADXL345 driver and a simulator
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
 * NETSHELL32-5, tech_spec.md sections 2.2.4 and 2.7. The driver's register
 * definitions and the I2C headers are private (REQ-FUNC-53). The simulator
 * seed is reachable only through private_include/accel_sim.h (REQ-FUNC-55,
 * REQ-SEC-03).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACCEL_SOURCE_SIMULATED = 0,
    ACCEL_SOURCE_HARDWARE,
} accel_source_t;

typedef struct {
    float          x_g, y_g, z_g; /* acceleration in g */
    int64_t        t_acq_us;      /* esp_timer time at acquisition */
    accel_source_t source;
} accel_sample_t;                 /* same struct for driver and simulator (REQ-FUNC-53) */

#define ESP_ERR_ACCEL_BASE        0xB400
#define ESP_ERR_ACCEL_NOT_PRESENT (ESP_ERR_ACCEL_BASE + 1) /* NACK on probe/transaction */
#define ESP_ERR_ACCEL_TIMEOUT     (ESP_ERR_ACCEL_BASE + 2) /* transaction exceeded the timeout */
#define ESP_ERR_ACCEL_BUS         (ESP_ERR_ACCEL_BASE + 3) /* other bus error, stuck bus */
#define ESP_ERR_ACCEL_IDENTITY    (ESP_ERR_ACCEL_BASE + 4) /* DEVID != 0xE5 */
#define ESP_ERR_ACCEL_CONFIG      (ESP_ERR_ACCEL_BASE + 5) /* config read-back mismatch */
#define ESP_ERR_ACCEL_READ        (ESP_ERR_ACCEL_BASE + 6) /* data burst read failed */
#define ESP_ERR_ACCEL_BUSY        (ESP_ERR_ACCEL_BASE + 7) /* sensor lock not obtained in time */

/* Creates the I2C bus, the sensor mutex and the probe task. A failure leaves
 * the simulator path fully working (REQ-REL-01); accel_sensor_read() never
 * depends on this succeeding. Call once at boot. */
esp_err_t accel_sensor_init(void);

/* Hardware sample if a sensor is active, otherwise a simulated sample. On a
 * hardware failure, returns the error and out->source = ACCEL_SOURCE_HARDWARE
 * so the caller can emit a per-slot ERR. ESP_ERR_INVALID_ARG if out is NULL. */
esp_err_t accel_sensor_read(accel_sample_t *out);

bool      accel_sensor_hw_active(void);
uint32_t  accel_sensor_max_age_ms(void); /* CONFIG_ACCEL_MAX_AGE_MS */

#ifdef __cplusplus
}
#endif
