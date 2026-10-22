/**
 * @file accel_select.c
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
 *
 * REQ-FUNC-44, 57, 58, REQ-PERF-05, REQ-HW-04, REQ-HW-08.
 */
#include "accel_select.h"

#include "adxl345.h"
#include "adxl345_regs.h"

static const uint8_t s_addrs[2] = { ADXL345_ADDR_PRIMARY, ADXL345_ADDR_ALTERNATE };

void accel_select_init(accel_sel_t *s, const accel_bus_ops_t *ops, void *bus, const accel_sel_env_t *env,
                       uint32_t timeout_ms, uint32_t fail_threshold)
{
    if (s == NULL) {
        return;
    }
    s->ops = ops;
    s->bus = bus;
    s->env = env;
    s->timeout_ms = timeout_ms;
    s->fail_threshold = (fail_threshold == 0u) ? 1u : fail_threshold;
    atomic_store(&s->state, (uint32_t)ACCEL_SEL_NO_SENSOR);
    atomic_store(&s->addr, 0u);
    atomic_store(&s->consecutive_fail, 0u);
    atomic_store(&s->fail_key[0], ACCEL_SEL_FAIL_KEY_NONE);
    atomic_store(&s->fail_key[1], ACCEL_SEL_FAIL_KEY_NONE);
    s->last_probe[0] = ACCEL_SEL_NO_RESULT;
    s->last_probe[1] = ACCEL_SEL_NO_RESULT;
}

accel_sel_state_t accel_select_state(accel_sel_t *s)
{
    if (s == NULL) {
        return ACCEL_SEL_NO_SENSOR;
    }
    return (accel_sel_state_t)atomic_load(&s->state);
}

uint8_t accel_select_addr(accel_sel_t *s)
{
    if (s == NULL) {
        return 0;
    }
    return (uint8_t)atomic_load(&s->addr);
}

static bool ready(const accel_sel_t *s)
{
    return s != NULL && s->ops != NULL && s->env != NULL && s->env->lock != NULL &&
           s->env->unlock != NULL && s->env->now_us != NULL;
}

static void notify(accel_sel_t *s, accel_sel_evt_t evt, uint8_t addr, esp_err_t err)
{
    if (s->env->notify != NULL) {
        s->env->notify(s->env->ctx, evt, addr, err);
    }
}

static void note_probe(accel_sel_t *s, int idx, esp_err_t err)
{
    if (s->last_probe[idx] != err) {
        s->last_probe[idx] = err;
        notify(s, ACCEL_SEL_EVT_PROBE, s_addrs[idx], err);
    }
}

/* Exactly one bus operation per lock acquisition. */
static esp_err_t locked_probe(accel_sel_t *s, uint8_t addr)
{
    if (!s->env->lock(s->env->ctx, ACCEL_SEL_LOCK_WAIT_MS)) {
        return ESP_ERR_ACCEL_BUSY;
    }
    esp_err_t err = s->ops->probe(s->bus, addr, s->timeout_ms);
    s->env->unlock(s->env->ctx);
    return err;
}

static esp_err_t locked_recover(accel_sel_t *s)
{
    if (s->ops->recover == NULL) {
        return ESP_OK;
    }
    if (!s->env->lock(s->env->ctx, ACCEL_SEL_LOCK_WAIT_MS)) {
        return ESP_ERR_ACCEL_BUSY;
    }
    esp_err_t err = s->ops->recover(s->bus);
    s->env->unlock(s->env->ctx);
    return err;
}

static esp_err_t locked_identity(accel_sel_t *s, uint8_t addr)
{
    const adxl345_t d = { .ops = s->ops, .bus = s->bus, .addr = addr, .timeout_ms = s->timeout_ms };
    if (!s->env->lock(s->env->ctx, ACCEL_SEL_LOCK_WAIT_MS)) {
        return ESP_ERR_ACCEL_BUSY;
    }
    esp_err_t err = adxl345_check_identity(&d);
    s->env->unlock(s->env->ctx);
    return err;
}

static esp_err_t locked_config_step(accel_sel_t *s, uint8_t addr, unsigned step)
{
    const adxl345_t d = { .ops = s->ops, .bus = s->bus, .addr = addr, .timeout_ms = s->timeout_ms };
    if (!s->env->lock(s->env->ctx, ACCEL_SEL_LOCK_WAIT_MS)) {
        return ESP_ERR_ACCEL_BUSY;
    }
    esp_err_t err = adxl345_config_step(&d, step);
    s->env->unlock(s->env->ctx);
    return err;
}

esp_err_t accel_select_probe_cycle(accel_sel_t *s)
{
    if (!ready(s) || s->ops->probe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (accel_select_state(s) == ACCEL_SEL_ACTIVE) {
        return ESP_OK;
    }

    bool timeout_seen = false;
    esp_err_t last_err = ESP_ERR_ACCEL_NOT_PRESENT;
    esp_err_t cfg_err = ESP_OK; /* identity / configuration failure, the more significant error */
    for (int i = 0; i < 2; i++) {
        const uint8_t addr = s_addrs[i];
        esp_err_t r = locked_probe(s, addr);
        note_probe(s, i, r);
        if (r != ESP_OK) {
            last_err = r;
            if (r == ESP_ERR_ACCEL_TIMEOUT) {
                timeout_seen = true;
            }
            continue;
        }

        atomic_store(&s->addr, (uint32_t)addr);
        atomic_store(&s->state, (uint32_t)ACCEL_SEL_CONFIGURING);
        esp_err_t err = locked_identity(s, addr);
        for (unsigned step = 0; err == ESP_OK && step < ADXL345_CFG_STEPS; step++) {
            err = locked_config_step(s, addr, step);
        }
        if (err == ESP_OK) {
            if (s->env->delay_ms != NULL) {
                s->env->delay_ms(s->env->ctx, ACCEL_SEL_SETTLE_MS); /* first conversion, lock not held */
            }
            atomic_store(&s->consecutive_fail, 0u);
            atomic_store(&s->fail_key[0], ACCEL_SEL_FAIL_KEY_NONE);
            atomic_store(&s->fail_key[1], ACCEL_SEL_FAIL_KEY_NONE);
            atomic_store(&s->state, (uint32_t)ACCEL_SEL_ACTIVE);
            notify(s, ACCEL_SEL_EVT_UP, addr, ESP_OK);
            return ESP_OK;
        }

        /* A wrong or flaky device at this address: report it once per (addr, reason)
         * and go on to the other address, where the real part may be. */
        atomic_store(&s->state, (uint32_t)ACCEL_SEL_NO_SENSOR);
        cfg_err = err;
        const uint32_t key = ((uint32_t)addr << 16) | ((uint32_t)err & 0xFFFFu);
        if (atomic_exchange(&s->fail_key[i], key) != key) {
            notify(s, ACCEL_SEL_EVT_FAIL, addr, err);
        }
    }
    if (timeout_seen) {
        (void)locked_recover(s); /* at most once per cycle (REQ-HW-08) */
    }
    return (cfg_err != ESP_OK) ? cfg_err : last_err;
}

void accel_probe_sched_init(accel_probe_sched_t *p, uint32_t interval_ms)
{
    if (p == NULL) {
        return;
    }
    p->interval_ms = interval_ms;
    p->last_start_us = 0;
    p->ever = false;
}

uint32_t accel_probe_wait_ms(const accel_probe_sched_t *p, int64_t now_us)
{
    if (p == NULL || !p->ever) {
        return 0; /* the first cycle runs at once */
    }
    const int64_t due = p->last_start_us + (int64_t)p->interval_ms * 1000;
    if (now_us >= due) {
        return 0;
    }
    return (uint32_t)((due - now_us + 999) / 1000);
}

bool accel_probe_step(accel_sel_t *s, accel_probe_sched_t *p, int64_t now_us)
{
    if (s == NULL || p == NULL || accel_select_state(s) == ACCEL_SEL_ACTIVE) {
        return false;
    }
    if (accel_probe_wait_ms(p, now_us) > 0) {
        return false;
    }
    p->ever = true;
    p->last_start_us = now_us;
    (void)accel_select_probe_cycle(s);
    return true;
}

esp_err_t accel_select_read(accel_sel_t *s, int16_t raw[3], int64_t *t_acq_us)
{
    if (!ready(s) || raw == NULL || t_acq_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (accel_select_state(s) != ACCEL_SEL_ACTIVE) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t addr = accel_select_addr(s);
    const adxl345_t d = { .ops = s->ops, .bus = s->bus, .addr = addr, .timeout_ms = s->timeout_ms };

    esp_err_t err;
    if (!s->env->lock(s->env->ctx, ACCEL_SEL_LOCK_WAIT_MS)) {
        err = ESP_ERR_ACCEL_BUSY;
    } else {
        err = adxl345_read_raw(&d, raw);
        if (err == ESP_OK) {
            *t_acq_us = s->env->now_us(s->env->ctx); /* right after the transaction returned */
        }
        s->env->unlock(s->env->ctx);
    }
    if (err == ESP_OK) {
        atomic_store(&s->consecutive_fail, 0u);
        return ESP_OK;
    }
    uint32_t fails = atomic_fetch_add(&s->consecutive_fail, 1u) + 1u;
    if (fails >= s->fail_threshold) {
        atomic_store(&s->state, (uint32_t)ACCEL_SEL_NO_SENSOR);
        atomic_store(&s->fail_key[0], ACCEL_SEL_FAIL_KEY_NONE); /* a later probe failure is a new report */
        atomic_store(&s->fail_key[1], ACCEL_SEL_FAIL_KEY_NONE);
        notify(s, ACCEL_SEL_EVT_FAIL, addr, err);
    }
    return err;
}
