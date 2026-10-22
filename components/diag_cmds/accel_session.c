/**
 * @file accel_session.c
 * @brief System-wide accel_xyz session: absolute-deadline slots on a dedicated task, cancel
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
 * REQ-FUNC-40..49, REQ-PERF-02, tech_spec.md 2.6.5 (D-04). There is NO mutex: the claim,
 * publication and cancel atomics live in accel_session_core.c (pure, unit-tested), and a
 * cancel names the session id it targets, so a late cancel can never hit a newer
 * session. diag_session_cancel() is safe on the BLE host task.
 */
#include "accel_session.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "accel_sched.h"
#include "accel_session_core.h"
#include "accel_sensor.h"
#include "diag_format.h"
#include "diag_internal.h"

static const char *TAG = DIAG_TAG;

static accel_session_core_t s_core;
static diag_ctx_t           s_owner_ctx; /* written by the claimant before publication */

static TaskHandle_t      s_task;
static StaticSemaphore_t s_done_buf;
static SemaphoreHandle_t s_done;
static char              s_line[DIAG_LINE_MAX];

static const char *reason_name(uint32_t why)
{
    switch (why) {
    case DIAG_CANCEL_DISCONNECT: return "disconnect";
    case DIAG_CANCEL_REBOOT:     return "reboot";
    case DIAG_CANCEL_TX_FAIL:    return "tx_fail";
    case DIAG_CANCEL_TIMEOUT:    return "timeout";
    default:                     return "unknown";
    }
}

void diag_session_cancel(diag_transport_t t, uint32_t gen, diag_cancel_reason_t why)
{
    if (accel_core_cancel(&s_core, t, gen, why) && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* Waits for the absolute deadline; false if a cancel for this session arrived. */
static bool wait_until(int64_t deadline_us, uint32_t sid, uint32_t *why)
{
    for (;;) {
        if (accel_core_cancelled(&s_core, sid, why)) {
            return false;
        }
        int64_t now = esp_timer_get_time();
        if (now >= deadline_us) {
            return true;
        }
        int64_t rem_ms = (deadline_us - now + 999) / 1000;
        if (rem_ms > 1000) {
            rem_ms = 1000;
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)rem_ms);
        if (ticks == 0) {
            ticks = 1;
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
}

static esp_err_t send_line(const diag_ctx_t *ctx)
{
    return ctx->write_line(ctx, s_line, strlen(s_line));
}

static void run_session(uint32_t sid)
{
    const diag_ctx_t ctx = s_owner_ctx; /* by value: no pointer into any stack */
    const char *src = diag_transport_name(ctx.transport);
    const int64_t t0 = esp_timer_get_time();
    uint32_t errors = 0;
    uint32_t done_slots = 0;
    uint32_t why = 0;
    bool was_cancelled = false;

    for (uint32_t n = 1; n <= ACCEL_SESSION_SAMPLES; n++) {
        if (!wait_until(accel_sched_deadline_us(t0, n), sid, &why)) {
            was_cancelled = true;
            break;
        }
        accel_sample_t smp;
        esp_err_t rerr = accel_sensor_read(&smp);
        const int64_t now = esp_timer_get_time();
        uint32_t age_ms = 0;
        const bool hw = (smp.source == ACCEL_SOURCE_HARDWARE);
        accel_slot_t slot = accel_sched_classify(hw, rerr, smp.t_acq_us, now, accel_sensor_max_age_ms(), &age_ms);

        esp_err_t ferr;
        switch (slot) {
        case ACCEL_SLOT_ERR_SENSOR:
            ferr = diag_fmt_accel_err(s_line, sizeof(s_line), "sensor_error", n);
            break;
        case ACCEL_SLOT_ERR_STALE:
            ferr = diag_fmt_accel_err(s_line, sizeof(s_line), "sensor_stale", n);
            break;
        default:
            ferr = diag_fmt_accel_data(s_line, sizeof(s_line), n, diag_g_to_mg(smp.x_g),
                                       diag_g_to_mg(smp.y_g), diag_g_to_mg(smp.z_g), age_ms,
                                       slot == ACCEL_SLOT_HW_DATA);
            break;
        }
        ESP_LOGI(TAG, "ACCEL_SAMPLE n=%" PRIu32 " src=%s age_ms=%" PRIu32 " t_ms=%" PRIu32, n,
                 accel_slot_is_error(slot) ? "error"
                                           : ((slot == ACCEL_SLOT_SIM_DATA) ? "simulated" : "hardware"),
                 age_ms, diag_now_ms());
        if (accel_slot_is_error(slot)) {
            errors++;
        }
        done_slots = n;
        if (ferr != ESP_OK) {
            ESP_LOGE(TAG, "accel line format failed (%s)", esp_err_to_name(ferr));
            why = DIAG_CANCEL_TX_FAIL;
            was_cancelled = true;
            break;
        }
        if (send_line(&ctx) != ESP_OK) {
            /* The only place a tx_fail cancel originates (tech_spec.md 2.5.5). */
            why = (ctx.is_current != NULL && !ctx.is_current(&ctx)) ? DIAG_CANCEL_DISCONNECT
                                                                    : DIAG_CANCEL_TX_FAIL;
            was_cancelled = true;
            break;
        }
    }

    const char *resmon_tag;
    if (!was_cancelled) {
        if (diag_fmt_accel_complete(s_line, sizeof(s_line), ACCEL_SESSION_SAMPLES, errors) == ESP_OK) {
            (void)send_line(&ctx);
        }
        ESP_LOGI(TAG, "ACCEL_SESSION_COMPLETE samples=%u errors=%" PRIu32 " t_ms=%" PRIu32 " src=%s",
                 (unsigned)ACCEL_SESSION_SAMPLES, errors, diag_now_ms(), src);
        resmon_tag = "session_complete";
    } else {
        ESP_LOGI(TAG, "ACCEL_SESSION_CANCEL reason=%s n=%" PRIu32 " src=%s t_ms=%" PRIu32,
                 reason_name(why), done_slots, src, diag_now_ms());
        resmon_tag = "session_cancel";
    }

    /* Release FIRST (session id cleared, then the claim; then the serial REPL is woken), and
     * only then run the resource monitor: its UART output must not hold the session claim
     * or the REPL prompt (ADV-IMPL-09). s_line is not used after this point. */
    accel_core_release(&s_core);
    if (ctx.transport == DIAG_TRANSPORT_SERIAL && s_done != NULL) {
        (void)xSemaphoreGive(s_done);
    }
    diag_resmon_emit(resmon_tag);
}

static void session_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); /* bounded; woken by start and cancel */
        uint32_t sid = accel_core_active_sid(&s_core);
        if (sid != 0u) {
            run_session(sid);
        }
    }
}

esp_err_t accel_session_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    accel_core_init(&s_core);
    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    if (s_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(session_task_fn, "accel_sess", CONFIG_DIAG_ACCEL_TASK_STACK, NULL,
                                            CONFIG_DIAG_ACCEL_TASK_PRIO, &s_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t accel_session_start(const diag_ctx_t *ctx)
{
    if (ctx == NULL || ctx->write_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!accel_core_try_claim(&s_core)) {
        return ESP_ERR_INVALID_STATE; /* the running session is not disturbed */
    }

    /* Owner fields are written before publication. */
    s_owner_ctx = *ctx;
    accel_core_set_owner(&s_core, ctx->transport, ctx->gen);
    if (ctx->transport == DIAG_TRANSPORT_SERIAL && s_done != NULL) {
        (void)xSemaphoreTake(s_done, 0); /* discard a stale give */
    }

    /* The start line is written BEFORE the session id is published: the session
     * task can be woken by a stale notification or its 1 s timeout at any time,
     * and once the id is published it would start using s_line and the sink
     * concurrently with this write. A cancel that arrives before publication is
     * not lost in effect: a gone link makes the first slot's write fail and the
     * session cancels itself, and a reboot ends everything anyway. */
    esp_err_t err = ESP_FAIL;
    if (diag_fmt_accel_start(s_line, sizeof(s_line)) == ESP_OK) {
        err = ctx->write_line(ctx, s_line, strlen(s_line));
    }
    if (err != ESP_OK) {
        accel_core_abort_unpublished(&s_core);
        return ESP_FAIL;
    }
    (void)accel_core_publish(&s_core);
    ESP_LOGI(TAG, "ACCEL_SESSION_START src=%s t_ms=%" PRIu32, diag_transport_name(ctx->transport), diag_now_ms());
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool accel_session_wait_done(uint32_t timeout_ms)
{
    if (s_done == NULL) {
        return false;
    }
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
