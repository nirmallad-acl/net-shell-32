/**
 * @file accel_sensor.c
 * @brief Sensor API: init, probe task, sensor mutex, public read (hardware or simulator)
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
 * REQ-FUNC-44, 53, 54, 55, 57, 58, REQ-PERF-05, REQ-REL-01, REQ-HW-07..10.
 * tech_spec.md section 2.7. A bus or sensor failure never prevents
 * accel_sensor_read() from returning a (simulated) sample.
 */
#include "accel_sensor.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "accel_bus_i2c.h"
#include "accel_select.h"
#include "accel_sim.h"
#include "adxl345.h"

static const char *TAG = "accel";

/* Simulator range sanity (REQ-FUNC-54): min <= max and within the +/-2 g range. */
_Static_assert(CONFIG_ACCEL_SIM_XY_MIN_MG <= CONFIG_ACCEL_SIM_XY_MAX_MG, "ACCEL_SIM_XY range inverted");
_Static_assert(CONFIG_ACCEL_SIM_Z_MIN_MG <= CONFIG_ACCEL_SIM_Z_MAX_MG, "ACCEL_SIM_Z range inverted");
_Static_assert(CONFIG_ACCEL_SIM_XY_MIN_MG >= -2000 && CONFIG_ACCEL_SIM_XY_MAX_MG <= 2000,
               "ACCEL_SIM_XY outside +/-2 g");
_Static_assert(CONFIG_ACCEL_SIM_Z_MIN_MG >= -2000 && CONFIG_ACCEL_SIM_Z_MAX_MG <= 2000,
               "ACCEL_SIM_Z outside +/-2 g");

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static TaskHandle_t      s_probe_task;
static bool              s_inited;

static accel_sel_t s_sel;
static accel_sim_t s_sim = { .state = 0x2545F491u };

static const accel_sim_range_t s_sim_range = {
    .xy_min_mg = CONFIG_ACCEL_SIM_XY_MIN_MG,
    .xy_max_mg = CONFIG_ACCEL_SIM_XY_MAX_MG,
    .z_min_mg = CONFIG_ACCEL_SIM_Z_MIN_MG,
    .z_max_mg = CONFIG_ACCEL_SIM_Z_MAX_MG,
};

/* ---- selector environment ---- */

static bool env_lock(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (t == 0) {
        t = 1;
    }
    return xSemaphoreTake(s_mtx, t) == pdTRUE;
}

static void env_unlock(void *ctx)
{
    (void)ctx;
    (void)xSemaphoreGive(s_mtx);
}

static int64_t env_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static const char *result_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                      return "found";
    case ESP_ERR_ACCEL_NOT_PRESENT:   return "not_present";
    case ESP_ERR_ACCEL_TIMEOUT:       return "timeout";
    case ESP_ERR_ACCEL_BUS:           return "bus_error";
    case ESP_ERR_ACCEL_IDENTITY:      return "wrong_identity";
    case ESP_ERR_ACCEL_CONFIG:        return "config_failed";
    case ESP_ERR_ACCEL_READ:          return "read_failed";
    case ESP_ERR_ACCEL_BUSY:          return "busy";
    default:                          return "error";
    }
}

static void env_notify(void *ctx, accel_sel_evt_t evt, uint8_t addr, esp_err_t err)
{
    (void)ctx;
    switch (evt) {
    case ACCEL_SEL_EVT_PROBE:
        ESP_LOGI(TAG, "SENSOR_PROBE addr=0x%02x result=%s", addr, result_name(err));
        break;
    case ACCEL_SEL_EVT_UP:
        ESP_LOGI(TAG, "SENSOR_UP addr=0x%02x", addr);
        /* A sensor is active: genuine bus errors must be visible again (ADV-IMPL-10). */
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        break;
    case ACCEL_SEL_EVT_FAIL:
        /* Called from the probe task (identity/config failure) AND from the session task
         * (read strikes): this callback must NOT wake the probe task, or the probe task
         * would wake itself and spin (REV-THRD-01). The read path wakes it instead
         * (accel_sensor_read). The bus error code is part of the reason token. */
        ESP_LOGW(TAG, "SENSOR_FAIL reason=%s addr=0x%02x", result_name(err), addr);
        esp_log_level_set("i2c.master", ESP_LOG_NONE); /* probing a NO_SENSOR bus again */
        break;
    default:
        break;
    }
}

static void env_delay_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay((t == 0) ? 1 : t); /* probe task only, never with the sensor lock held */
}

static const accel_sel_env_t s_env = {
    .delay_ms = env_delay_ms,
    .ctx = NULL,
    .lock = env_lock,
    .unlock = env_unlock,
    .now_us = env_now_us,
    .notify = env_notify,
};

/* ---- probe task (priority 1: probing has no deadline, REQ-PERF-05) ---- */

static accel_probe_sched_t s_probe_sched;

static TickType_t ms_to_ticks_min1(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0) ? 1 : t;
}

/* The task only waits and calls the pure accel_probe_step(): a cycle starts at most once
 * per CONFIG_ACCEL_PROBE_INTERVAL_MS however often the task is woken (REV-THRD-01). */
static void probe_task_fn(void *arg)
{
    (void)arg;
    accel_probe_sched_init(&s_probe_sched, CONFIG_ACCEL_PROBE_INTERVAL_MS);
    for (;;) {
        if (accel_select_state(&s_sel) == ACCEL_SEL_ACTIVE) {
            /* Bounded wait; the read path wakes us early when the sensor drops out. */
            (void)ulTaskNotifyTake(pdTRUE, ms_to_ticks_min1(CONFIG_ACCEL_PROBE_INTERVAL_MS));
            continue;
        }
        uint32_t wait_ms = accel_probe_wait_ms(&s_probe_sched, esp_timer_get_time());
        if (wait_ms > 0) {
            (void)ulTaskNotifyTake(pdTRUE, ms_to_ticks_min1(wait_ms));
            continue;
        }
        (void)accel_probe_step(&s_sel, &s_probe_sched, esp_timer_get_time());
        (void)ulTaskNotifyTake(pdTRUE, 0); /* drop any wake that arrived during the cycle */
    }
}

esp_err_t accel_sensor_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Production seed: once, from the hardware RNG. The stream continues, so
     * consecutive sessions differ (REQ-FUNC-55). */
    accel_sim_seed(&s_sim, esp_random());

    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (s_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const accel_bus_ops_t *ops = NULL;
    void *bus = NULL;
    esp_err_t err = accel_bus_i2c_create(&ops, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus unavailable (%s); simulator-only operation", esp_err_to_name(err));
        return err;
    }
    /* H-12 / H-15: with no sensor attached every probe NACKs or times out, and the
     * IDF I2C master driver logs those at ERROR level. An absent sensor is a normal
     * state here (reported once as SENSOR_PROBE at INFO), so the driver's own tag is
     * silenced. Genuine bring-up failures above were already logged (before this
     * line) and the selector reports every state change itself. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    accel_select_init(&s_sel, ops, bus, &s_env, CONFIG_ACCEL_I2C_TIMEOUT_MS, CONFIG_ACCEL_FAIL_THRESHOLD);

    BaseType_t ok = xTaskCreatePinnedToCore(probe_task_fn, "sensor_probe", CONFIG_ACCEL_PROBE_TASK_STACK,
                                            NULL, CONFIG_ACCEL_PROBE_TASK_PRIO, &s_probe_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_probe_task = NULL;
        accel_bus_i2c_destroy(); /* do not leave GPIO21/22, the bus and 2 device handles allocated */
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        ESP_LOGW(TAG, "probe task creation failed; simulator-only operation");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t accel_sensor_read(accel_sample_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_inited && accel_select_state(&s_sel) == ACCEL_SEL_ACTIVE) {
        int16_t raw[3] = { 0, 0, 0 };
        int64_t t_acq = 0;
        esp_err_t err = accel_select_read(&s_sel, raw, &t_acq);
        out->source = ACCEL_SOURCE_HARDWARE;
        if (err != ESP_OK) {
            /* A failing hardware slot is reported as such, never relabelled as simulated. */
            out->t_acq_us = esp_timer_get_time();
            if (s_probe_task != NULL && accel_select_state(&s_sel) != ACCEL_SEL_ACTIVE) {
                xTaskNotifyGive(s_probe_task); /* dropped out: re-probe sooner (called on the session task) */
            }
            return err;
        }
        out->x_g = adxl345_raw_to_g(raw[0]);
        out->y_g = adxl345_raw_to_g(raw[1]);
        out->z_g = adxl345_raw_to_g(raw[2]);
        out->t_acq_us = t_acq;
        return ESP_OK;
    }

    /* No sensor: simulated sample, no lock, no bus access. */
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    accel_sim_next_mg(&s_sim, &s_sim_range, &x, &y, &z);
    out->x_g = (float)x / 1000.0f;
    out->y_g = (float)y / 1000.0f;
    out->z_g = (float)z / 1000.0f;
    out->t_acq_us = esp_timer_get_time();
    out->source = ACCEL_SOURCE_SIMULATED;
    return ESP_OK;
}

bool accel_sensor_hw_active(void)
{
    return s_inited && accel_select_state(&s_sel) == ACCEL_SEL_ACTIVE;
}

uint32_t accel_sensor_max_age_ms(void)
{
    return CONFIG_ACCEL_MAX_AGE_MS;
}
