/**
 * @file adxl345_regs.h
 * @brief Private: ADXL345 register map and the fixed configuration values
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
 * Values per the ADXL345 datasheet; configuration per tech_spec.md 2.7.3.
 */
#pragma once

#define ADXL345_ADDR_PRIMARY   0x53u /* SDO (ALT ADDRESS) low  */
#define ADXL345_ADDR_ALTERNATE 0x1Du /* SDO (ALT ADDRESS) high */

#define ADXL345_REG_DEVID       0x00u
#define ADXL345_REG_BW_RATE     0x2Cu
#define ADXL345_REG_POWER_CTL   0x2Du
#define ADXL345_REG_INT_ENABLE  0x2Eu
#define ADXL345_REG_DATA_FORMAT 0x31u
#define ADXL345_REG_DATAX0      0x32u /* DATAX0..DATAZ1: 6 bytes, little-endian per axis */
#define ADXL345_REG_FIFO_CTL    0x38u

#define ADXL345_DEVID_VALUE     0xE5u

#define ADXL345_POWER_STANDBY   0x00u
#define ADXL345_POWER_MEASURE   0x08u
#define ADXL345_BW_RATE_100HZ   0x0Au /* 100 Hz output data rate, normal power */
#define ADXL345_DATA_FMT_FULL_RES_2G 0x08u /* FULL_RES = 1, right-justified, range +/-2 g */
#define ADXL345_FIFO_BYPASS     0x00u
#define ADXL345_INT_NONE        0x00u

/* Coherent sample burst: one read of 0x2C..0x37 (12 bytes) returns BW_RATE, POWER_CTL,
 * INT_ENABLE, INT_MAP, INT_SOURCE, DATA_FORMAT and the 6 data bytes in one transaction,
 * so the configuration is verified together with every sample. (INT_SOURCE is cleared by
 * the read; interrupts are not used.) */
#define ADXL345_BURST_LEN       13u /* 0x2C..0x38: also FIFO_CTL, so the FIFO mode is verified too */
#define ADXL345_BURST_OFF_BW    0u
#define ADXL345_BURST_OFF_PWR   1u
#define ADXL345_BURST_OFF_FMT   5u
#define ADXL345_BURST_OFF_DATA  6u
#define ADXL345_BURST_OFF_FIFO  12u

/* POWER_CTL: bit 3 = Measure, bit 2 = Sleep. A valid running part has Measure set and Sleep clear. */
#define ADXL345_POWER_MODE_MASK 0x0Cu
/* FIFO_CTL bits [7:6] = FIFO_MODE; 0 = Bypass (what configuration writes). */
#define ADXL345_FIFO_MODE_MASK  0xC0u

/* FULL_RES scale: 3.9 mg/LSB typical. */
#define ADXL345_G_PER_LSB       0.0039f
