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

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

#include "ble_host_client.h"
#include "ble_ota_gate.h"
#include "ble_ota_health.h"
#include "ble_ota_internal.h"
#include "ble_ota_proto.h"
#include "ble_ota_transport.h"

/* The two public reboot-guard codes must not collide with the private codes
 * (+1..+11) nor with the ble_host codes (REQ-INT-07). */
_Static_assert(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE > ESP_ERR_BLE_OTA_NOT_RUNNING &&
               ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY > ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE &&
               ESP_ERR_BLE_OTA_REBOOT_STALE > ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY,
               "public reboot-guard codes collide with private ble_ota codes");
_Static_assert(ESP_ERR_BLE_HOST_BASE > ESP_ERR_BLE_OTA_REBOOT_STALE + 0xE0,
               "ble_host and ble_ota error bases overlap");

static const char *TAG = BLE_OTA_TAG_FACADE;

static bool      s_inited;
static esp_err_t s_init_result = ESP_FAIL;
/* The OTA client registration is recorded SEPARATELY and never changes
 * s_init_result, so the H-4 health inputs are unchanged (REQ-INT-09). */
static esp_err_t s_ota_reg_result = ESP_FAIL;

/* ---- reboot guard ops (production wiring of the pure guard in ble_ota_gate.c) ---- */

static bool op_gen_current_secure(uint32_t gen)
{
    uint32_t g = 0;
    bool secure = false;
    return ble_host_get_link(NULL, &g, &secure, NULL) == ESP_OK && g == gen && secure;
}

/* If the OTA client is not registered, no OTA RX can ever reach the protocol, so it is
 * IDLE and not stuck by construction. Answering without touching the protocol also avoids
 * its (possibly never created) mutex when ble_ota_proto_init() failed (ADV-IMPL-08). */
static bool op_proto_idle(void)
{
    if (!ble_ota_transport_is_registered()) {
        return true;
    }
    return strcmp(ble_ota_proto_state_name(), "IDLE") == 0;
}

static bool op_proto_drain_stuck(void)
{
    if (!ble_ota_transport_is_registered()) {
        return false;
    }
    return ble_ota_proto_is_drain_stuck();
}

static bool op_pending_verify(void)
{
    if (ble_ota_health_is_pending_verify()) {
        return true;
    }
    if (s_init_result != ESP_OK) {
        /* ble_ota_init() failed: fall back to asking the OTA data directly. */
        const esp_partition_t *p = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (p != NULL && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            return true;
        }
    }
    return false;
}

static const ble_ota_guard_ops_t s_guard_ops = {
    .exec = ble_host_exec,
    .gen_current_secure = op_gen_current_secure,
    .proto_idle = op_proto_idle,
    .proto_drain_stuck = op_proto_drain_stuck,
    .pending_verify = op_pending_verify,
};

esp_err_t ble_ota_reboot_guard_commit(uint32_t ble_gen, uint32_t timeout_ms, bool *out_override)
{
    ble_ota_gate_set_guard_ops(&s_guard_ops);
    return ble_ota_gate_guard_commit(ble_gen, timeout_ms, out_override);
}

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

    /* NETSHELL32-5: register the OTA GATT client with ble_host, at most once per
     * boot (ADV-OTA-11), right after the protocol layer it attaches to. The result
     * is logged by the transport and kept apart from s_init_result. */
    ble_ota_gate_set_guard_ops(&s_guard_ops);
    s_ota_reg_result = ble_ota_transport_register();
    if (s_ota_reg_result != ESP_OK) {
        ESP_LOGW(TAG, "OTA client not registered (%s); fwupgrade start will refuse",
                 esp_err_to_name(s_ota_reg_result));
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
