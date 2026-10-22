/**
 * @file accel_select.h
 * @brief PURE: sensor state machine (probe, configure, active, fail counting) over injected lock/clock
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
 * tech_spec.md section 2.7.5. Every bus transaction and every recover() call
 * is made under the injected lock, one operation per acquisition (REQ-PERF-05),
 * with a 60 ms lock wait. No other lock is taken while it is held.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "accel_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACCEL_SEL_LOCK_WAIT_MS 60u
#define ACCEL_SEL_NO_RESULT    (-1) /* sentinel: nothing logged yet for this address */

typedef enum {
    ACCEL_SEL_NO_SENSOR = 0,
    ACCEL_SEL_CONFIGURING,
    ACCEL_SEL_ACTIVE,
} accel_sel_state_t;

typedef enum {
    ACCEL_SEL_EVT_PROBE = 0, /* a probe result changed for addr (err) */
    ACCEL_SEL_EVT_UP,        /* sensor became ACTIVE at addr */
    ACCEL_SEL_EVT_FAIL,      /* sensor left ACTIVE / configuration failed (err) */
} accel_sel_evt_t;

typedef struct {
    void *ctx;
    bool    (*lock)(void *ctx, uint32_t timeout_ms);
    void    (*unlock)(void *ctx);
    int64_t (*now_us)(void *ctx);
    void    (*notify)(void *ctx, accel_sel_evt_t evt, uint8_t addr, esp_err_t err);
    void    (*delay_ms)(void *ctx, uint32_t ms); /* optional; called WITHOUT the lock held */
} accel_sel_env_t;

/* Settling time after POWER_CTL = measure before the first sample may be reported: the
 * first conversion at 100 Hz takes 10 ms, so a read right after configuration could return
 * all-zero data that would be labelled source=hardware (R2-02). */
#define ACCEL_SEL_SETTLE_MS 20u

typedef struct {
    const accel_bus_ops_t *ops;
    void                  *bus;
    const accel_sel_env_t *env;
    uint32_t               timeout_ms;
    uint32_t               fail_threshold;
    _Atomic uint32_t       state; /* accel_sel_state_t */
    _Atomic uint32_t       addr;
    _Atomic uint32_t       consecutive_fail;
    _Atomic uint32_t       fail_key[2];   /* per probed address: (addr << 16) | (err & 0xFFFF) of the last
                                             logged probe-cycle failure, so two wrong parts do not make
                                             each other's report look new on every cycle */
    esp_err_t              last_probe[2];
} accel_sel_t;

#define ACCEL_SEL_FAIL_KEY_NONE 0xFFFFFFFFu

void accel_select_init(accel_sel_t *s, const accel_bus_ops_t *ops, void *bus, const accel_sel_env_t *env,
                       uint32_t timeout_ms, uint32_t fail_threshold);

accel_sel_state_t accel_select_state(accel_sel_t *s);
uint8_t           accel_select_addr(accel_sel_t *s);

/* One probe cycle (probe task only): probe 0x53 then 0x1D, identity, configuration.
 * A device that ACKs but fails identity or configuration does NOT stop the cycle: the
 * other address is still tried (a real part may sit there). EVT_FAIL for such a failure
 * is raised only when its (addr, reason) differs from the last one logged, so a
 * permanently wrong device is reported once. At most one bus recover() per cycle, only
 * if a probe timed out. Returns ESP_OK if the sensor is ACTIVE afterwards, otherwise the
 * identity/config error if there was one, else the last probe error. */
esp_err_t accel_select_probe_cycle(accel_sel_t *s);

/* Spacing of probe cycles (REQ-FUNC-57, REQ-PERF-05): however often the probe task is
 * woken, two cycles start at least interval_ms apart, so a failing cycle can never
 * re-arm itself into a tight loop. Pure so it is host-tested. */
typedef struct {
    uint32_t interval_ms;
    int64_t  last_start_us;
    bool     ever;
} accel_probe_sched_t;

void     accel_probe_sched_init(accel_probe_sched_t *p, uint32_t interval_ms);
/* Milliseconds until the next cycle may start (0 = now). */
uint32_t accel_probe_wait_ms(const accel_probe_sched_t *p, int64_t now_us);
/* Runs one cycle if the sensor is not ACTIVE and the spacing allows it. true if it ran. */
bool     accel_probe_step(accel_sel_t *s, accel_probe_sched_t *p, int64_t now_us);

/* One burst read (session task). ESP_ERR_INVALID_STATE if not ACTIVE (the caller
 * then uses the simulator and does not touch the bus). On failure the error is
 * returned and the consecutive-failure count rises; at fail_threshold the state
 * returns to NO_SENSOR and an EVT_FAIL is issued. */
esp_err_t accel_select_read(accel_sel_t *s, int16_t raw[3], int64_t *t_acq_us);

#ifdef __cplusplus
}
#endif
