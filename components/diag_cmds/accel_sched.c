/**
 * @file accel_sched.c
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
 *
 * REQ-FUNC-40, 41, 44, 45, 56.
 */
#include "accel_sched.h"

#include <stddef.h>

int64_t accel_sched_deadline_us(int64_t t0_us, uint32_t n)
{
    if (n < 1u || n > ACCEL_SESSION_SAMPLES) {
        return t0_us;
    }
    return t0_us + (int64_t)(n - 1u) * ACCEL_SESSION_INTERVAL_US;
}

uint32_t accel_sched_age_ms(int64_t t_acq_us, int64_t now_us)
{
    int64_t d = now_us - t_acq_us;
    if (d <= 0) {
        return 0;
    }
    int64_t ms = d / 1000;
    if (ms > (int64_t)UINT32_MAX) {
        ms = (int64_t)UINT32_MAX;
    }
    return (uint32_t)ms;
}

accel_slot_t accel_sched_classify(bool hardware, esp_err_t read_err, int64_t t_acq_us, int64_t now_us,
                                  uint32_t max_age_ms, uint32_t *age_ms_out)
{
    uint32_t age = accel_sched_age_ms(t_acq_us, now_us);
    if (age_ms_out != NULL) {
        *age_ms_out = age;
    }
    if (!hardware) {
        return ACCEL_SLOT_SIM_DATA;
    }
    if (read_err != ESP_OK) {
        return ACCEL_SLOT_ERR_SENSOR;
    }
    if (age > max_age_ms) {
        return ACCEL_SLOT_ERR_STALE;
    }
    return ACCEL_SLOT_HW_DATA;
}

bool accel_slot_is_error(accel_slot_t s)
{
    return s == ACCEL_SLOT_ERR_SENSOR || s == ACCEL_SLOT_ERR_STALE;
}
