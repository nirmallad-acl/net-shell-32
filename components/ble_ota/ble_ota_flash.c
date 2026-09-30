/**
 * @file ble_ota_flash.c
 * @brief L3: OTA write backend, worker task and event queue
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
 *
 * No BLE-stack header is included anywhere in this file (NFR-403). Every
 * esp_ota_* or esp_partition_* call in the whole component lives here.
 */
#include "ble_ota_flash.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ble_ota_internal.h"

static const char *TAG = BLE_OTA_TAG_FLASH;

#define FLASH_EVT_QUEUE_DEPTH 8
#define ERASE_CHUNK_BYTES     (64u * 1024u) /* FR-510: bounded per-call erase */

/* NFR-105: static, not a per-transfer heap allocation. This is what a
 * single esp_ota_write() call consumes (tech_spec.md §5.6). */
static uint8_t s_part_buf[CONFIG_BLE_OTA_MAX_PART_SIZE];

static QueueHandle_t                s_evt_q;
static TaskHandle_t                 s_worker_task;
static ble_ota_flash_done_fn        s_done_cb;
static ble_ota_flash_abort_query_fn s_abort_query;

static esp_ota_handle_t       s_handle;
static bool                   s_handle_open;
static const esp_partition_t *s_target;
static uint32_t               s_declared_size;
static uint32_t               s_used_bytes;

esp_err_t ble_ota_flash_init(void)
{
    if (s_evt_q != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_evt_q = xQueueCreate(FLASH_EVT_QUEUE_DEPTH, sizeof(ble_ota_flash_evt_t));
    if (s_evt_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ble_ota_flash_attach(ble_ota_flash_done_fn done_cb, ble_ota_flash_abort_query_fn abort_query)
{
    if (done_cb == NULL || abort_query == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_done_cb = done_cb;
    s_abort_query = abort_query;
    return ESP_OK;
}

static void report(ble_ota_flash_result_kind_t kind, esp_err_t err, uint16_t part_index)
{
    if (s_done_cb == NULL) {
        return;
    }
    ble_ota_flash_result_t r = {
        .kind = kind,
        .err = err,
        .part_index = part_index,
        .bytes_committed = s_used_bytes,
    };
    s_done_cb(&r);
}

static void do_begin(const ble_ota_flash_evt_t *evt)
{
    if (s_handle_open) {
        /* 0xFD discards any previously buffered/partial update. */
        esp_ota_abort(s_handle);
        s_handle_open = false;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no inactive OTA partition available");
        report(BLE_OTA_FLASH_RESULT_BEGIN_FAIL, ESP_ERR_NOT_FOUND, 0);
        return;
    }
    if (target == running) {
        /* FR-503 defence in depth: never write the active partition. */
        ESP_LOGE(TAG, "refusing to target the running partition");
        report(BLE_OTA_FLASH_RESULT_BEGIN_FAIL, ESP_ERR_INVALID_STATE, 0);
        return;
    }

    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        report(BLE_OTA_FLASH_RESULT_BEGIN_FAIL, err, 0);
        return;
    }

    s_handle_open = true;
    s_target = target;
    s_declared_size = evt->len;
    s_used_bytes = 0;
    ESP_LOGI(TAG, "FWUPG_TARGET_SLOT label=%s size=%" PRIu32, target->label, (uint32_t)target->size);
    report(BLE_OTA_FLASH_RESULT_BEGIN_OK, ESP_OK, 0);
}

static void do_commit_part(const ble_ota_flash_evt_t *evt)
{
    if (!s_handle_open) {
        ESP_LOGE(TAG, "commit_part with no open OTA handle");
        report(BLE_OTA_FLASH_RESULT_PART_FAIL, ESP_ERR_INVALID_STATE, evt->count);
        return;
    }

    /* Exactly one esp_ota_write() per part (FR-501, §5.6) -- the single
     * call site for this whole component. */
    esp_err_t err = esp_ota_write(s_handle, s_part_buf, evt->len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed for part %u: %s", evt->count, esp_err_to_name(err));
        report(BLE_OTA_FLASH_RESULT_PART_FAIL, err, evt->count);
        return;
    }

    s_used_bytes += evt->len;
    ESP_LOGI(TAG, "FWUPG_PART_COMMIT idx=%u len=%" PRIu32 " total=%" PRIu32,
             evt->count, evt->len, s_used_bytes);
    report(BLE_OTA_FLASH_RESULT_PART_OK, ESP_OK, evt->count);
}

static void do_finalize(const ble_ota_flash_evt_t *evt)
{
    if (!s_handle_open) {
        report(BLE_OTA_FLASH_RESULT_FINALIZE_FAIL, ESP_ERR_INVALID_STATE, evt->count);
        return;
    }

    /* FR-407: cumulative bytes written must match the 0xFE-declared size
     * before finalization is even attempted. */
    if (s_used_bytes != s_declared_size) {
        ESP_LOGE(TAG, "size mismatch at finalize: declared=%" PRIu32 " received=%" PRIu32,
                  s_declared_size, s_used_bytes);
        esp_ota_abort(s_handle);
        s_handle_open = false;
        report(BLE_OTA_FLASH_RESULT_FINALIZE_FAIL, ESP_ERR_INVALID_SIZE, evt->count);
        return;
    }

    esp_err_t err = esp_ota_end(s_handle); /* consumes the handle either way (FR-506) */
    s_handle_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FWUPG_FINALIZE_FAIL err=%s", esp_err_to_name(err));
        report(BLE_OTA_FLASH_RESULT_FINALIZE_FAIL, err, evt->count);
        return;
    }

    err = esp_ota_set_boot_partition(s_target); /* FR-507: only after finalization succeeds */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        report(BLE_OTA_FLASH_RESULT_FINALIZE_FAIL, err, evt->count);
        return;
    }

    ESP_LOGI(TAG, "FWUPG_FINALIZE_OK");
    ESP_LOGI(TAG, "FWUPG_BOOTSET label=%s", s_target->label);
    report(BLE_OTA_FLASH_RESULT_FINALIZE_OK, ESP_OK, evt->count);
}

static void do_format(void)
{
    if (s_handle_open) {
        esp_ota_abort(s_handle);
        s_handle_open = false;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        report(BLE_OTA_FLASH_RESULT_FORMAT_FAIL, ESP_ERR_NOT_FOUND, 0);
        return;
    }

    size_t offset = 0;
    esp_err_t err = ESP_OK;
    while (offset < target->size) {
        size_t chunk = target->size - offset;
        if (chunk > ERASE_CHUNK_BYTES) {
            chunk = ERASE_CHUNK_BYTES;
        }
        err = esp_partition_erase_range(target, offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase failed at offset 0x%x: %s", (unsigned)offset, esp_err_to_name(err));
            break;
        }
        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(1)); /* FR-510: never hold both cores for the whole range */
    }

    if (err != ESP_OK) {
        report(BLE_OTA_FLASH_RESULT_FORMAT_FAIL, err, 0);
        return;
    }

    s_used_bytes = 0;
    ESP_LOGI(TAG, "format complete: label=%s size=%" PRIu32, target->label, (uint32_t)target->size);
    report(BLE_OTA_FLASH_RESULT_FORMAT_OK, ESP_OK, 0);
}

static void do_abort(void)
{
    if (s_handle_open) {
        esp_err_t err = esp_ota_abort(s_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ota_abort returned %s", esp_err_to_name(err));
        }
        s_handle_open = false;
    }
    s_used_bytes = 0;
    report(BLE_OTA_FLASH_RESULT_ABORTED, ESP_OK, 0);
}

static void worker_task_fn(void *arg)
{
    (void)arg;
    ble_ota_flash_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_q, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (evt.type != BLE_OTA_FLASH_EVT_ABORT &&
            s_abort_query != NULL && s_abort_query()) {
            /* An abort was requested after this event was queued (§5.9.4):
             * drop it without touching flash. The EVT_ABORT that was
             * queued behind it (FIFO, never overtaken) runs next. */
            ESP_LOGW(TAG, "dropping queued flash event type=%d: abort pending", (int)evt.type);
            continue;
        }
        switch (evt.type) {
        case BLE_OTA_FLASH_EVT_BEGIN:       do_begin(&evt); break;
        case BLE_OTA_FLASH_EVT_COMMIT_PART: do_commit_part(&evt); break;
        case BLE_OTA_FLASH_EVT_FINALIZE:    do_finalize(&evt); break;
        case BLE_OTA_FLASH_EVT_FORMAT:      do_format(); break;
        case BLE_OTA_FLASH_EVT_ABORT:       do_abort(); break;
        default:
            ESP_LOGE(TAG, "unknown flash event type %d", (int)evt.type);
            break;
        }
    }
}

esp_err_t ble_ota_flash_start(void)
{
    if (s_worker_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_evt_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain any stale events from a previous session before starting. */
    ble_ota_flash_evt_t stale;
    while (xQueueReceive(s_evt_q, &stale, 0) == pdTRUE) {
        /* discard */
    }

    BaseType_t ok = xTaskCreatePinnedToCore(worker_task_fn, "ble_ota_wrk",
                                             CONFIG_BLE_OTA_WORKER_TASK_STACK, NULL,
                                             CONFIG_BLE_OTA_WORKER_TASK_PRIO,
                                             &s_worker_task, 1 /* core 1 */);
    if (ok != pdPASS) {
        s_worker_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_ota_flash_stop(void)
{
    if (s_worker_task != NULL) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }
    if (s_handle_open) {
        esp_ota_abort(s_handle);
        s_handle_open = false;
    }
    s_used_bytes = 0;
}

bool ble_ota_flash_is_running(void)
{
    return s_worker_task != NULL;
}

esp_err_t ble_ota_flash_post_event(const ble_ota_flash_evt_t *evt, TickType_t timeout_ticks)
{
    if (evt == NULL || s_evt_q == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    BaseType_t ok = xQueueSend(s_evt_q, evt, timeout_ticks);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint8_t *ble_ota_flash_get_part_buffer(void)
{
    return s_part_buf;
}

size_t ble_ota_flash_get_part_buffer_size(void)
{
    return sizeof(s_part_buf);
}

esp_err_t ble_ota_flash_get_target_slot(const char **out_label, size_t *out_size)
{
    if (out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_label != NULL) {
        *out_label = target->label;
    }
    *out_size = target->size;
    return ESP_OK;
}

uint32_t ble_ota_flash_get_used_bytes(void)
{
    return s_used_bytes;
}
