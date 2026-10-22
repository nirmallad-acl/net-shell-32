/**
 * @file accel_sim.h
 * @brief Private: seedable xorshift32 acceleration simulator
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
 * PURE. accel_sim_seed() is declared ONLY here: no public API, CLI command or
 * BLE path can reach it (REQ-FUNC-55, REQ-SEC-03). The production seed comes
 * from esp_random() once at init (accel_sensor.c); tests seed it explicitly.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state; /* never 0 */
} accel_sim_t;

typedef struct {
    int32_t xy_min_mg;
    int32_t xy_max_mg;
    int32_t z_min_mg;
    int32_t z_max_mg;
} accel_sim_range_t;

/* A seed of 0 is replaced by a fixed non-zero constant (xorshift32 must not be 0). */
void accel_sim_seed(accel_sim_t *s, uint32_t seed);

/* Next raw xorshift32 output (never 0). */
uint32_t accel_sim_rand(accel_sim_t *s);

/* Uniform integer milli-g: x and y in [xy_min, xy_max], z in [z_min, z_max]. */
void accel_sim_next_mg(accel_sim_t *s, const accel_sim_range_t *r,
                       int32_t *x_mg, int32_t *y_mg, int32_t *z_mg);

#ifdef __cplusplus
}
#endif
