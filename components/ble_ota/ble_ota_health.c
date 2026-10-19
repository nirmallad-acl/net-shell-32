/**
 * @file ble_ota_health.c
 * @brief Post-boot health confirmation, rollback decision and recovery
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-09-09
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
 * Implements tech_spec.md §7 in full: the five-check conjunction (§7.2),
 * the recoverability-class split (class R = H-1/H-4, class D = H-2/H-3/
 * H-5), the RB-INV invariant (a class-R failure is never marked valid),
 * and the §7.3.1 terminal decision table.
 *
 * Judgment call (documented, see final report): H-1 (console/REPL) and
 * H-3 (Wi-Fi STA) inputs are recorded from main.c via
 * ble_ota_health_record_boot_status(), because NFR-303 forbids adding a
 * getter to net_cli's public header and none already exists there.
 *
 * KNOWN, ACCEPTED LIMITATION (not a defect -- reviewed and accepted at
 * Gate 2/Phase 4, see tech_spec.md §7.3.2): §7.3.2's "retry
 * esp_console_new_repl_uart()+esp_console_start_repl() for a failed H-1"
 * is a no-op by design in this implementation. main.c owns the sole
 * esp_console_repl_t handle and exposes no recreate primitive, and
 * net_cli's public API has no such primitive either -- adding one was
 * explicitly out of scope for this feature (the user chose not to scope
 * that net_cli follow-up now). Only the narrow corner case of (single
 * bootable OTA slot AND a failed console) is affected by this gap, and
 * that case already terminates in "serial re-flash required" regardless
 * of whether a retry is attempted -- H-1's retry attempt cannot change
 * the outcome, since there is no other recovery channel to fall back to.
 * The gap therefore only forfeits a chance at self-healing a *transient*
 * H-1 failure; it does not affect correctness or safety of the rollback
 * decision itself (RB-INV still holds: a class-R failure, H-1 included,
 * is never marked valid). H-4 (BLE-OTA module) retry has no such
 * limitation and is fully implemented below.
 */
#include "ble_ota_health.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ble_ota.h" /* ble_ota_init()/ble_ota_register_all(), used by attempt_recovery() */
#include "ble_ota_internal.h"

static const char *TAG = BLE_OTA_TAG_HEALTH;

#define NVS_NAMESPACE   "ble_ota"
#define NVS_KEY_RB_CNT  "rb_count"
#define NVS_KEY_RB_MASK "rb_mask"
#define NVS_KEY_BOOTCNT "boot_cnt"

#define HEALTH_TICK_MS      5000u
#define HEALTH_TASK_STACK   3072u
#define HEALTH_TASK_PRIO    3u

typedef struct {
    ble_ota_health_class_t klass;
    const char            *name;
} health_check_meta_t;

static const health_check_meta_t s_check_meta[BLE_OTA_HEALTH_CHECK_COUNT] = {
    [BLE_OTA_HEALTH_H1_CONSOLE] = { BLE_OTA_HEALTH_CLASS_R, "H-1(console)" },
    [BLE_OTA_HEALTH_H2_NVS]     = { BLE_OTA_HEALTH_CLASS_D, "H-2(nvs)" },
    [BLE_OTA_HEALTH_H3_WIFI]    = { BLE_OTA_HEALTH_CLASS_D, "H-3(wifi)" },
    [BLE_OTA_HEALTH_H4_BLE_OTA] = { BLE_OTA_HEALTH_CLASS_R, "H-4(ble_ota)" },
    [BLE_OTA_HEALTH_H5_HEAP]    = { BLE_OTA_HEALTH_CLASS_D, "H-5(heap)" },
};

/* ---- boot-time facts recorded from outside (main.c, ble_ota.c) ---- */
static esp_err_t s_wifi_init         = ESP_FAIL;
static esp_err_t s_wifi_set_mode     = ESP_FAIL;
static esp_err_t s_wifi_start        = ESP_FAIL;
static esp_err_t s_console_new_repl  = ESP_FAIL;
static esp_err_t s_console_start_repl = ESP_FAIL;
static esp_err_t s_ble_ota_init_res  = ESP_FAIL;
static esp_err_t s_ble_ota_reg_res   = ESP_FAIL;
static volatile bool s_boot_status_recorded    = false;
static volatile bool s_ble_ota_status_recorded = false;

/* ---- health-evaluation state ---- */
static bool               s_pending_verify = false;
static volatile bool      s_validated      = false;
static esp_timer_handle_t s_tick_timer     = NULL;
static TaskHandle_t       s_health_task    = NULL;
static int64_t            s_pv_detect_us   = 0;

ble_ota_health_class_t ble_ota_health_check_class(ble_ota_health_check_id_t id)
{
    if (id >= BLE_OTA_HEALTH_CHECK_COUNT) {
        return BLE_OTA_HEALTH_CLASS_R; /* fail safe: unknown id treated as fatal-class */
    }
    return s_check_meta[id].klass;
}

const char *ble_ota_health_check_name(ble_ota_health_check_id_t id)
{
    if (id >= BLE_OTA_HEALTH_CHECK_COUNT) {
        return "H-?";
    }
    return s_check_meta[id].name;
}

void ble_ota_health_record_boot_status(esp_err_t wifi_init,
                                        esp_err_t wifi_set_mode,
                                        esp_err_t wifi_start,
                                        esp_err_t console_new_repl,
                                        esp_err_t console_start_repl)
{
    s_wifi_init          = wifi_init;
    s_wifi_set_mode      = wifi_set_mode;
    s_wifi_start         = wifi_start;
    s_console_new_repl   = console_new_repl;
    s_console_start_repl = console_start_repl;
    s_boot_status_recorded = true;
}

void ble_ota_health_record_ble_ota_status(esp_err_t init_result, esp_err_t register_result)
{
    s_ble_ota_init_res = init_result;
    s_ble_ota_reg_res   = register_result;
    s_ble_ota_status_recorded = true;
}

bool ble_ota_health_is_pending_verify(void)
{
    return s_pending_verify && !s_validated;
}

uint32_t ble_ota_health_remaining_deadline_s(void)
{
    if (!ble_ota_health_is_pending_verify()) {
        return 0;
    }
    int64_t elapsed_s = (esp_timer_get_time() - s_pv_detect_us) / 1000000;
    int64_t remaining = (int64_t)CONFIG_BLE_OTA_HEALTH_DEADLINE_S - elapsed_s;
    return (remaining > 0) ? (uint32_t)remaining : 0u;
}

/* ---- individual checks ---- */

static bool check_h1_console(void)
{
    return s_boot_status_recorded &&
           s_console_new_repl == ESP_OK &&
           s_console_start_repl == ESP_OK;
}

static bool check_h2_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "H-2 probe: nvs_open failed (%s)", esp_err_to_name(err));
        return false;
    }
    uint32_t cnt = 0;
    /* Missing key on first ever boot is expected, not a failure. */
    esp_err_t get_err = nvs_get_u32(h, NVS_KEY_BOOTCNT, &cnt);
    if (get_err != ESP_OK && get_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "H-2 probe: nvs_get_u32 failed (%s)", esp_err_to_name(get_err));
        nvs_close(h);
        return false;
    }
    cnt++;
    esp_err_t set_err = nvs_set_u32(h, NVS_KEY_BOOTCNT, cnt);
    esp_err_t commit_err = (set_err == ESP_OK) ? nvs_commit(h) : set_err;
    nvs_close(h);
    if (set_err != ESP_OK || commit_err != ESP_OK) {
        ESP_LOGW(TAG, "H-2 probe: write/commit failed (set=%s commit=%s)",
                  esp_err_to_name(set_err), esp_err_to_name(commit_err));
        return false;
    }
    return true;
}

static bool check_h3_wifi(void)
{
    return s_boot_status_recorded &&
           s_wifi_init == ESP_OK &&
           s_wifi_set_mode == ESP_OK &&
           s_wifi_start == ESP_OK;
}

static bool check_h4_ble_ota(void)
{
    return s_ble_ota_status_recorded &&
           s_ble_ota_init_res == ESP_OK &&
           s_ble_ota_reg_res == ESP_OK;
}

static bool check_h5_heap(void)
{
    uint32_t free_heap = esp_get_free_internal_heap_size();
    uint32_t floor_bytes = (uint32_t)CONFIG_BLE_OTA_HEALTH_MIN_FREE_HEAP_KB * 1024u;
    return free_heap >= floor_bytes;
}

static bool run_check(ble_ota_health_check_id_t id)
{
    switch (id) {
    case BLE_OTA_HEALTH_H1_CONSOLE: return check_h1_console();
    case BLE_OTA_HEALTH_H2_NVS:     return check_h2_nvs();
    case BLE_OTA_HEALTH_H3_WIFI:    return check_h3_wifi();
    case BLE_OTA_HEALTH_H4_BLE_OTA: return check_h4_ble_ota();
    case BLE_OTA_HEALTH_H5_HEAP:    return check_h5_heap();
    default:                        return false;
    }
}

/* ---- NVS-backed rollback bookkeeping (best-effort: H-2's own caveat,
 * tech_spec.md §7.2 -- if NVS is unwritable the counter degrades to
 * "every boot looks like the first", which the class-R terminal policy
 * is deliberately written to remain correct without). ---- */

static void rb_counters_load(uint8_t *out_count, uint8_t *out_mask)
{
    *out_count = 0;
    *out_mask = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    (void)nvs_get_u8(h, NVS_KEY_RB_CNT, out_count);
    (void)nvs_get_u8(h, NVS_KEY_RB_MASK, out_mask);
    nvs_close(h);
}

static void rb_counters_store(uint8_t count, uint8_t mask)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rollback counter persist skipped: nvs_open failed (%s)",
                  esp_err_to_name(err));
        return;
    }
    (void)nvs_set_u8(h, NVS_KEY_RB_CNT, count);
    (void)nvs_set_u8(h, NVS_KEY_RB_MASK, mask);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void rb_counters_clear(void)
{
    /* tech_spec.md §10.4(4): "the rollback counter read/increment/reset
     * in NVS ... the value before and after". This is the reset half; the
     * increment half is logged at its one call site in evaluate_once(). */
    ESP_LOGI(TAG, "FWUPG_DECIDE point=rb_counter_nvs inputs=reset outcome=after=0");
    rb_counters_store(0, 0);
}

/* ---- §7.3.2 bounded recovery-channel re-init ---- */

static void attempt_recovery(uint8_t failed_mask)
{
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (failed_mask & (1u << BLE_OTA_HEALTH_H1_CONSOLE)) {
            /* KNOWN, ACCEPTED LIMITATION (see file header and
             * tech_spec.md §7.3.2): main.c owns the only
             * esp_console_repl_t handle and exposes no recreate API, so
             * this retry is a no-op by design, not a bug. Only affects
             * the (single bootable slot + failed console) corner case,
             * which already ends in "serial re-flash required" either
             * way -- this forfeits a chance at self-healing a transient
             * failure, nothing more; it does not change RB-INV or any
             * safety property. Logged honestly rather than silently
             * skipped. */
            ESP_LOGE(TAG, "FWUPG_RECOVER_ATTEMPT ch=H-1 n=%d err=%s",
                      attempt, esp_err_to_name(ESP_ERR_NOT_SUPPORTED));
        }
        if (failed_mask & (1u << BLE_OTA_HEALTH_H4_BLE_OTA)) {
            esp_err_t init_err = ble_ota_init();
            esp_err_t reg_err = ESP_FAIL;
            if (init_err == ESP_OK || init_err == ESP_ERR_INVALID_STATE) {
                reg_err = ble_ota_register_all();
            }
            ble_ota_health_record_ble_ota_status(init_err, reg_err);
            ESP_LOGE(TAG, "FWUPG_RECOVER_ATTEMPT ch=H-4 n=%d err=%s",
                      attempt, esp_err_to_name(reg_err));
        }
        if (check_h1_console() && check_h4_ble_ota()) {
            return; /* re-run of the full conjunction happens in the caller */
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void stop_timer_locked(void)
{
    if (s_tick_timer != NULL) {
        esp_timer_stop(s_tick_timer);
    }
}

static void evaluate_once(void)
{
    if (s_validated || !s_pending_verify) {
        return;
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    bool checks[BLE_OTA_HEALTH_CHECK_COUNT];
    bool all_pass = true;
    uint8_t failed_mask = 0;
    bool contains_class_r = false;

    for (int i = 0; i < BLE_OTA_HEALTH_CHECK_COUNT; i++) {
        checks[i] = run_check((ble_ota_health_check_id_t)i);
        ble_ota_health_class_t klass = ble_ota_health_check_class((ble_ota_health_check_id_t)i);
        if (checks[i]) {
            ESP_LOGD(TAG, "FWUPG_HEALTH_PASS check=%s", ble_ota_health_check_name(i));
        } else {
            all_pass = false;
            failed_mask |= (1u << i);
            if (klass == BLE_OTA_HEALTH_CLASS_R) {
                contains_class_r = true;
            }
            /* tech_spec.md §10.4(4): the recoverability class of any
             * failure must be present on the fail line, not just derivable
             * elsewhere. */
            ESP_LOGW(TAG, "FWUPG_HEALTH_FAIL check=%s class=%s", ble_ota_health_check_name(i),
                     (klass == BLE_OTA_HEALTH_CLASS_R) ? "R" : "D");
        }
    }

    if (uptime_s >= (uint32_t)CONFIG_BLE_OTA_HEALTH_MIN_UPTIME_S && all_pass) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s",
                      esp_err_to_name(err));
            return; /* try again next tick, still within the deadline */
        }
        ESP_LOGW(TAG, "FWUPG_VALIDATED uptime=%" PRIu32, uptime_s);
        rb_counters_clear();
        s_validated = true;
        stop_timer_locked();
        return;
    }

    if (uptime_s < (uint32_t)CONFIG_BLE_OTA_HEALTH_DEADLINE_S) {
        return; /* keep re-evaluating until both hold or the deadline expires */
    }

    /* Deadline reached with the conjunction unmet: §7.3 step 4 / §7.3.1.
     * tech_spec.md §10.4(4): this "validate-or-rollback" decision point
     * must be recoverable from uptime, the failed-check mask AND the
     * counter value -- rb_count is read/incremented/persisted first so
     * this one line carries all three. */
    uint8_t rb_count = 0, rb_mask = 0;
    rb_counters_load(&rb_count, &rb_mask);
    uint8_t rb_count_before = rb_count;
    if (rb_count < 255) {
        rb_count++;
    }
    rb_mask = failed_mask;
    rb_counters_store(rb_count, rb_mask);
    ESP_LOGI(TAG, "FWUPG_DECIDE point=rb_counter_nvs inputs=before=%u outcome=after=%u",
              rb_count_before, rb_count);
    ESP_LOGE(TAG, "FWUPG_ROLLBACK_TRIGGERED reason=health_deadline uptime=%" PRIu32
                  " failed=0x%02x count=%u", uptime_s, failed_mask, rb_count);

    bool possible = esp_ota_check_rollback_is_possible();
    uint8_t applicable_limit = contains_class_r ? (uint8_t)CONFIG_BLE_OTA_ROLLBACK_HARD_LIMIT
                                                 : (uint8_t)CONFIG_BLE_OTA_ROLLBACK_LOOP_LIMIT;
    if (rb_count >= applicable_limit) {
        /* tech_spec.md §10.2: rollback limit reached; the §7.3.1 terminal
         * decision table (logged just below, per branch) is about to run. */
        ESP_LOGW(TAG, "FWUPG_ROLLBACK_LOOP_DETECTED count=%u", rb_count);
    }

    if (!contains_class_r) {
        /* class-D-only failure */
        if (rb_count < (uint8_t)CONFIG_BLE_OTA_ROLLBACK_LOOP_LIMIT) {
            ESP_LOGE(TAG, "FWUPG_DECIDE point=rollback_loop_table class=D count=%u limit=%u possible=%d outcome=rollback",
                      rb_count, (unsigned)CONFIG_BLE_OTA_ROLLBACK_LOOP_LIMIT, (int)possible);
            esp_ota_mark_app_invalid_rollback_and_reboot(); /* reboots; does not return on success */
            ESP_LOGE(TAG, "esp_ota_mark_app_invalid_rollback_and_reboot did not reboot");
            return;
        }
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "FWUPG_ROLLBACK_LOOP_ESCAPE class=D failed=0x%02x possible=%d (%s)",
                  failed_mask, (int)possible, esp_err_to_name(err));
        rb_counters_clear();
        s_validated = true;
        stop_timer_locked();
        return;
    }

    /* RB-INV: contains a class-R failure -- never marked valid, ever. */
    if (possible && rb_count < (uint8_t)CONFIG_BLE_OTA_ROLLBACK_HARD_LIMIT) {
        const char *marker = (rb_count < (uint8_t)CONFIG_BLE_OTA_ROLLBACK_LOOP_LIMIT)
                                  ? "FWUPG_ROLLBACK_TRIGGERED"
                                  : "FWUPG_ROLLBACK_PREFER_OTHER";
        ESP_LOGE(TAG, "%s class=R failed=0x%02x count=%u possible=%d", marker, failed_mask, rb_count, (int)possible);
        esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "esp_ota_mark_app_invalid_rollback_and_reboot did not reboot");
        return;
    }

    const char *reason = possible ? "hard_limit" : "no_target";
    ESP_LOGE(TAG, "FWUPG_UNRECOVERABLE failed=0x%02x reason=%s possible=%d", failed_mask, reason, (int)possible);
    attempt_recovery(failed_mask);
    /* Re-run the conjunction once more in case recovery actually fixed
     * something; otherwise stay PENDING_VERIFY and let the bootloader's
     * own rollback (FR-806) or an operator's serial re-flash be the
     * eventual exit. Either way we do not reboot or mark valid here. */
}

static void health_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEALTH_TICK_MS * 2));
        evaluate_once();
    }
}

static void tick_timer_cb(void *arg)
{
    (void)arg;
    if (s_health_task != NULL) {
        xTaskNotifyGive(s_health_task);
    }
}

esp_err_t ble_ota_health_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_get_state_partition failed (%s); treating as normal boot",
                  esp_err_to_name(err));
        return ESP_OK;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "FWUPG_BOOT state=%d", (int)state);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "FWUPG_PENDING_VERIFY");
    s_pending_verify = true;
    s_pv_detect_us = esp_timer_get_time();

    BaseType_t task_ok = xTaskCreate(health_task_fn, "ble_ota_hlth", HEALTH_TASK_STACK,
                                      NULL, HEALTH_TASK_PRIO, &s_health_task);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create health task");
        s_health_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = {
        .callback = tick_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_ota_hlth_tick",
    };
    err = esp_timer_create(&targs, &s_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed (%s)", esp_err_to_name(err));
        vTaskDelete(s_health_task);
        s_health_task = NULL;
        return err;
    }

    err = esp_timer_start_periodic(s_tick_timer, (uint64_t)HEALTH_TICK_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed (%s)", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
