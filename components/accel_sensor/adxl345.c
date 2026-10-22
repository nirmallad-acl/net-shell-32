/**
 * @file adxl345.c
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
 *
 * REQ-FUNC-50 (identity before any write, configuration sequence),
 * REQ-FUNC-51 (coherent burst read, decode, scale), REQ-FUNC-52.
 */
#include "adxl345.h"

#include <stdbool.h>

#include "adxl345_regs.h"

/* Bus operations are contracted to return ESP_OK or an ESP_ERR_ACCEL_* code.
 * Anything else is folded into the given fallback so no foreign code escapes. */
static esp_err_t norm(esp_err_t e, esp_err_t fallback)
{
    if (e == ESP_OK) {
        return ESP_OK;
    }
    if (e >= ESP_ERR_ACCEL_NOT_PRESENT && e <= ESP_ERR_ACCEL_BUSY) {
        return e;
    }
    return fallback;
}

static bool dev_valid(const adxl345_t *d)
{
    return d != NULL && d->ops != NULL && d->ops->read_regs != NULL && d->ops->write_reg != NULL;
}

esp_err_t adxl345_check_identity(const adxl345_t *d)
{
    if (!dev_valid(d)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t id = 0;
    esp_err_t err = norm(d->ops->read_regs(d->bus, d->addr, ADXL345_REG_DEVID, &id, 1, d->timeout_ms),
                         ESP_ERR_ACCEL_BUS);
    if (err != ESP_OK) {
        return err;
    }
    return (id == ADXL345_DEVID_VALUE) ? ESP_OK : ESP_ERR_ACCEL_IDENTITY;
}

static esp_err_t write_one(const adxl345_t *d, uint8_t reg, uint8_t val)
{
    return norm(d->ops->write_reg(d->bus, d->addr, reg, val, d->timeout_ms), ESP_ERR_ACCEL_BUS);
}

static esp_err_t verify_one(const adxl345_t *d, uint8_t reg, uint8_t expect)
{
    uint8_t v = 0;
    esp_err_t err = norm(d->ops->read_regs(d->bus, d->addr, reg, &v, 1, d->timeout_ms), ESP_ERR_ACCEL_BUS);
    if (err != ESP_OK) {
        return err;
    }
    return (v == expect) ? ESP_OK : ESP_ERR_ACCEL_CONFIG;
}

esp_err_t adxl345_config_step(const adxl345_t *d, unsigned step)
{
    if (!dev_valid(d) || step >= ADXL345_CFG_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (step) {
    case 0: return write_one(d, ADXL345_REG_POWER_CTL, ADXL345_POWER_STANDBY);
    case 1: return write_one(d, ADXL345_REG_BW_RATE, ADXL345_BW_RATE_100HZ);
    case 2: return write_one(d, ADXL345_REG_DATA_FORMAT, ADXL345_DATA_FMT_FULL_RES_2G);
    case 3: return write_one(d, ADXL345_REG_FIFO_CTL, ADXL345_FIFO_BYPASS);
    case 4: return write_one(d, ADXL345_REG_INT_ENABLE, ADXL345_INT_NONE);
    case 5: return write_one(d, ADXL345_REG_POWER_CTL, ADXL345_POWER_MEASURE);
    case 6: return verify_one(d, ADXL345_REG_BW_RATE, ADXL345_BW_RATE_100HZ);
    default: return verify_one(d, ADXL345_REG_DATA_FORMAT, ADXL345_DATA_FMT_FULL_RES_2G);
    }
}

void adxl345_decode(const uint8_t raw[6], int16_t xyz[3])
{
    for (int i = 0; i < 3; i++) {
        uint16_t u = (uint16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
        xyz[i] = (int16_t)u; /* the device sign-extends right-justified data */
    }
}

float adxl345_raw_to_g(int16_t raw)
{
    return (float)raw * ADXL345_G_PER_LSB;
}

esp_err_t adxl345_read_raw(const adxl345_t *d, int16_t xyz[3])
{
    if (!dev_valid(d) || xyz == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* One coherent burst 0x2C..0x38: the configuration registers that prove the part is still
     * configured and measuring (BW_RATE, POWER_CTL measure-and-not-sleep, DATA_FORMAT, FIFO
     * bypass), plus the data. A part that reset or went to
     * standby (POWER_CTL = 0, data = 0) must never be reported as fresh hardware data. */
    uint8_t buf[ADXL345_BURST_LEN];
    esp_err_t err = norm(d->ops->read_regs(d->bus, d->addr, ADXL345_REG_BW_RATE, buf, sizeof(buf),
                                           d->timeout_ms),
                         ESP_ERR_ACCEL_READ);
    if (err != ESP_OK) {
        return err;
    }
    if (buf[ADXL345_BURST_OFF_BW] != ADXL345_BW_RATE_100HZ ||
        (buf[ADXL345_BURST_OFF_PWR] & ADXL345_POWER_MODE_MASK) != ADXL345_POWER_MEASURE || /* measure, not sleep */
        buf[ADXL345_BURST_OFF_FMT] != ADXL345_DATA_FMT_FULL_RES_2G ||
        (buf[ADXL345_BURST_OFF_FIFO] & ADXL345_FIFO_MODE_MASK) != 0u) { /* bypass, as configured */
        return ESP_ERR_ACCEL_CONFIG; /* counted as a strike by the selector; xyz is not written */
    }
    adxl345_decode(&buf[ADXL345_BURST_OFF_DATA], xyz);
    return ESP_OK;
}
