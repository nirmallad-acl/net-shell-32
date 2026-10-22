/**
 * @file diag_resmon.c
 * @brief Resource monitor: HEAP_STAT and STACK_HWM log lines
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
 * REQ-MEM-02/03/04, REQ-TEST-04. HEAP_STAT free= uses
 * esp_get_free_internal_heap_size(), the exact function the health check H-5
 * uses (ble_ota_health.c), so every heap measurement is comparable (V-02).
 * Emitted 30 s after boot, on accel session complete/cancel and, via the BLE
 * worker, on BLE disconnect. Test hook: only log lines, no behaviour change.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "diag_internal.h"

static const char *TAG = DIAG_TAG;

#define RESMON_BOOT_DELAY_US (30ULL * 1000000ULL)

/* The pool all three HEAP_STAT numbers describe. It is EXACTLY the pool that
 * esp_get_free_internal_heap_size() counts in IDF v6.0.2
 * (components/esp_system/port/esp_system_chip.c: heap_caps_get_free_size(MALLOC_CAP_8BIT |
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)) and that the H-5 health check uses. A plain
 * MALLOC_CAP_INTERNAL also counts the 32-bit-only IRAM region, which made min_free larger
 * than free (DEF-1). With one pool, min_free <= free holds by construction. */
#define RESMON_HEAP_CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

/* Task names to report. Names of IDF-owned tasks are verification item V-01. */
static const char *const s_task_names[] = {
    "nimble_host", "blecli_wrk", "accel_sess", "sensor_probe", "ble_ota_notify", "ble_ota_wrk",
    "console_repl", "main",
};

static void (*s_provider)(uint32_t *cb_max_us, uint32_t *ota_rx_max_us);
static esp_timer_handle_t s_boot_timer;

void diag_resmon_set_provider(void (*fn)(uint32_t *cb_max_us, uint32_t *ota_rx_max_us))
{
    s_provider = fn;
}

void diag_resmon_emit(const char *why)
{
    if (why == NULL) {
        why = "unspecified";
    }
    uint32_t cb_max = 0;
    uint32_t ota_max = 0;
    if (s_provider != NULL) {
        s_provider(&cb_max, &ota_max);
    }
    ESP_LOGI(TAG, "HEAP_STAT free=%" PRIu32 " min_free=%" PRIu32 " largest=%" PRIu32 " cb_max_us=%" PRIu32
                  " ota_rx_max_us=%" PRIu32 " why=%s",
             esp_get_free_internal_heap_size(),
             (uint32_t)heap_caps_get_minimum_free_size(RESMON_HEAP_CAPS),
             (uint32_t)heap_caps_get_largest_free_block(RESMON_HEAP_CAPS), cb_max, ota_max, why);

    for (size_t i = 0; i < sizeof(s_task_names) / sizeof(s_task_names[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(s_task_names[i]);
        if (h != NULL) {
            ESP_LOGI(TAG, "STACK_HWM task=%s hwm=%u", s_task_names[i], (unsigned)uxTaskGetStackHighWaterMark(h));
        }
    }
}

static void boot_timer_cb(void *arg)
{
    (void)arg;
    diag_resmon_emit("boot_30s");

    /* Test hook H-9 (additive, INFO): NVS headroom for REQ-MEM-06. */
    nvs_stats_t st;
    esp_err_t err = nvs_get_stats(NULL, &st);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS_STAT used=%u free=%u total=%u", (unsigned)st.used_entries,
                 (unsigned)st.free_entries, (unsigned)st.total_entries);
    } else {
        ESP_LOGW(TAG, "NVS_STAT unavailable (%s)", esp_err_to_name(err));
    }
}

esp_err_t diag_resmon_init(void)
{
    if (s_boot_timer != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_timer_create_args_t args = {
        .callback = boot_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "diag_resmon",
    };
    esp_err_t err = esp_timer_create(&args, &s_boot_timer);
    if (err != ESP_OK) {
        s_boot_timer = NULL;
        return err;
    }
    err = esp_timer_start_once(s_boot_timer, RESMON_BOOT_DELAY_US);
    if (err != ESP_OK) {
        (void)esp_timer_delete(s_boot_timer);
        s_boot_timer = NULL;
    }
    return err;
}
