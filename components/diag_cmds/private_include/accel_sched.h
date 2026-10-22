/**
 * @file accel_sched.h
 * @brief PURE: accel_xyz deadlines, slot classification and error counting
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

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACCEL_SESSION_SAMPLES     15u
#define ACCEL_SESSION_INTERVAL_US 1000000

typedef enum {
    ACCEL_SLOT_SIM_DATA = 0, /* DATA ... source=simulated */
    ACCEL_SLOT_HW_DATA,      /* DATA ... source=hardware */
    ACCEL_SLOT_ERR_SENSOR,   /* ERR sensor_error */
    ACCEL_SLOT_ERR_STALE,    /* ERR sensor_stale */
} accel_slot_t;

/* Absolute deadline of slot n (1..15): t0 + (n-1) * 1 s, so there is no
 * cumulative drift. n outside 1..15 returns t0. */
int64_t accel_sched_deadline_us(int64_t t0_us, uint32_t n);

/* Whole milliseconds between acquisition and reporting, floored at 0. */
uint32_t accel_sched_age_ms(int64_t t_acq_us, int64_t now_us);

/* Classifies one slot. A simulated source is never an error. A hardware sample
 * with an error is ERR_SENSOR; a hardware sample older than max_age_ms is
 * ERR_STALE (age exactly at the limit is accepted, REQ-FUNC-56). *age_ms_out
 * (may be NULL) receives the age. */
accel_slot_t accel_sched_classify(bool hardware, esp_err_t read_err, int64_t t_acq_us, int64_t now_us,
                                  uint32_t max_age_ms, uint32_t *age_ms_out);

bool accel_slot_is_error(accel_slot_t s);

#ifdef __cplusplus
}
#endif
