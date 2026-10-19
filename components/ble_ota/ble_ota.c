/**
 * @file ble_ota.c
 * @brief Facade: component init and console-command registration entry points
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
#include "ble_ota.h"

#include "esp_log.h"

#include "ble_ota_health.h"
#include "ble_ota_internal.h"
#include "ble_ota_proto.h"

static const char *TAG = BLE_OTA_TAG_FACADE;

static bool      s_inited;
static esp_err_t s_init_result = ESP_FAIL;

esp_err_t ble_ota_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* L2 first: creates the mutex/semaphore/event-queue primitives that
     * MUST exist before any BLE or flash path can run (tech_spec.md
     * §5.9.3), and does not touch the BLE controller (FR-101). */
    esp_err_t err = ble_ota_proto_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_ota_proto_init failed: %s", esp_err_to_name(err));
        s_init_result = err;
        return err;
    }

    /* FR-809: evaluated on every boot, independent of BLE. */
    err = ble_ota_health_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_ota_health_init failed: %s", esp_err_to_name(err));
        s_init_result = err;
        return err;
    }

    s_init_result = ESP_OK;
    s_inited = true;
    return ESP_OK;
}

esp_err_t ble_ota_register_all(void)
{
    esp_err_t err = ble_ota_cmd_fwupgrade_register();
    /* H-4 input: this component is only truly "up" once both its own
     * init and its console registration have succeeded. */
    ble_ota_health_record_ble_ota_status(s_init_result, err);
    return err;
}

void ble_ota_note_boot_status(esp_err_t wifi_init,
                               esp_err_t wifi_set_mode,
                               esp_err_t wifi_start,
                               esp_err_t console_new_repl,
                               esp_err_t console_start_repl)
{
    ble_ota_health_record_boot_status(wifi_init, wifi_set_mode, wifi_start,
                                       console_new_repl, console_start_repl);
}
