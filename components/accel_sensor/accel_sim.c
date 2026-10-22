/**
 * @file accel_sim.c
 * @brief PURE: seedable xorshift32 acceleration simulator
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
 * REQ-FUNC-54, REQ-FUNC-55.
 */
#include "accel_sim.h"

#include <stddef.h>

#define SIM_DEFAULT_SEED 0x2545F491u

void accel_sim_seed(accel_sim_t *s, uint32_t seed)
{
    if (s == NULL) {
        return;
    }
    s->state = (seed != 0u) ? seed : SIM_DEFAULT_SEED;
}

uint32_t accel_sim_rand(accel_sim_t *s)
{
    if (s == NULL) {
        return 0;
    }
    uint32_t x = s->state;
    if (x == 0u) {
        x = SIM_DEFAULT_SEED;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->state = x;
    return x;
}

static int32_t draw(accel_sim_t *s, int32_t lo, int32_t hi)
{
    if (hi < lo) {
        int32_t t = lo;
        lo = hi;
        hi = t;
    }
    uint32_t span = (uint32_t)(hi - lo) + 1u; /* the range is bounded to +/-2000 mg by the callers */
    return lo + (int32_t)(accel_sim_rand(s) % span);
}

void accel_sim_next_mg(accel_sim_t *s, const accel_sim_range_t *r,
                       int32_t *x_mg, int32_t *y_mg, int32_t *z_mg)
{
    if (s == NULL || r == NULL || x_mg == NULL || y_mg == NULL || z_mg == NULL) {
        return;
    }
    *x_mg = draw(s, r->xy_min_mg, r->xy_max_mg);
    *y_mg = draw(s, r->xy_min_mg, r->xy_max_mg);
    *z_mg = draw(s, r->z_min_mg, r->z_max_mg);
}
