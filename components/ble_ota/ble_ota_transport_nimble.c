/**
 * @file ble_ota_transport_nimble.c
 * @brief L1: NimBLE transport -- the ONLY translation unit in this component that includes a NimBLE header
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
 * Implements FR-101..105 (lifecycle), FR-201..206 (GATT service and
 * advertising), FR-301..306 (pairing/bonding/runtime security check) and
 * OQ-9 (bounded re-advertise window). Delivers raw RX frames downward to
 * L2 via ble_ota_proto_rx() and registers this layer's notify/link-ctl
 * callbacks via ble_ota_proto_attach() (tech_spec.md §3.3).
 */
#include "ble_ota_transport.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#include "ble_ota_internal.h"
#include "ble_ota_health.h"
#include "ble_ota_proto.h"
#include "ble_ota_proto_defs.h"

static const char *TAG = BLE_OTA_TAG_BLE;

#define NOTIFY_QUEUE_DEPTH  4
#define NOTIFY_TASK_STACK   3072
#define NOTIFY_TASK_PRIO    5
#define NOTIFY_RETRY_COUNT  3
#define NOTIFY_RETRY_MS     100

#define RX_BUF_MAX (2u + CONFIG_BLE_OTA_MAX_CHUNK_SIZE)

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(BLE_OTA_SVC_UUID128_BYTES_LE);
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(BLE_OTA_RX_CHR_UUID128_BYTES_LE);
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(BLE_OTA_TX_CHR_UUID128_BYTES_LE);

static uint16_t s_tx_val_handle;
static uint8_t  s_own_addr_type;

static volatile bool s_running;
static volatile bool s_advertising;
static volatile bool s_connected;
static uint16_t      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static char           s_peer_addr_str[18];
static bool           s_peer_bonded;

static bool s_mem_released_once;

static QueueHandle_t s_notify_q;
static TaskHandle_t  s_notify_task;
static SemaphoreHandle_t s_host_task_done;

static esp_timer_handle_t s_readv_timer;

typedef struct {
    uint8_t len;
    uint8_t data[BLE_OTA_NOTIFY_MAX_LEN];
} notify_item_t;

/* ---- forward decls ---- */
static void start_advertising(void);
static void stop_advertising(const char *reason);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---- FR-301 runtime security check + downward RX delivery ---- */

static bool conn_is_secure(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return false;
    }
    return desc.sec_state.encrypted && desc.sec_state.bonded;
}

static int gatt_access_rx(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    if (total_len == 0 || total_len > RX_BUF_MAX) {
        /* FR-302: rejected before any field is read -- no partial
         * processing, no state change, no flash access. */
        ESP_LOGW(TAG, "FWUPG_RX_REJECT reason=oversized_frame len=%u", (unsigned)total_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[RX_BUF_MAX];
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    bool secure = conn_is_secure(conn_handle);
    ble_ota_proto_rx(secure, buf, out_len);
    return 0;
}

static int gatt_access_tx(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* Notify-only: no direct read/write is ever legal on this attribute. */
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_gatt_chrs[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = gatt_access_rx,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_tx_uuid.u,
        .access_cb = gatt_access_tx,
        .val_handle = &s_tx_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }, /* terminator */
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_gatt_chrs,
    },
    { 0 }, /* terminator */
};

/* ---- upward callbacks registered with L2 (tech_spec.md §3.3) ---- */

static esp_err_t notify_fn_impl(const uint8_t *frame, size_t len)
{
    if (s_notify_q == NULL || len == 0 || len > BLE_OTA_NOTIFY_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    notify_item_t item;
    item.len = (uint8_t)len;
    memcpy(item.data, frame, len);
    if (xQueueSend(s_notify_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "notify queue full; dropping frame op=0x%02x", frame[0]);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void link_ctl_fn_impl(ble_ota_link_ctl_cmd_t cmd)
{
    if (cmd == BLE_OTA_LINK_CTL_DISCONNECT && s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void notify_task_fn(void *arg)
{
    (void)arg;
    notify_item_t item;
    for (;;) {
        if (xQueueReceive(s_notify_q, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (!s_connected) {
            continue; /* nobody to notify */
        }
        int rc = -1;
        for (int attempt = 0; attempt < NOTIFY_RETRY_COUNT; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(item.data, item.len);
            if (om == NULL) {
                rc = BLE_HS_ENOMEM;
            } else {
                rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
            }
            if (rc == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "notify send failed after retries: rc=%d op=0x%02x", rc, item.data[0]);
        }
    }
}

/* ---- advertising ---- */

static void readv_timer_cb(void *arg)
{
    (void)arg;
    if (!s_connected && s_advertising) {
        stop_advertising("readvertise_window_expired");
    }
}

static void start_advertising(void)
{
    /* tech_spec.md OQ-2: flags (3B) + complete 128-bit service UUID (18B) =
     * 21B fits the 31B legacy AD payload, but adding the complete device
     * name on top does not (21B + 2B header + strlen("NETSHELL32")=10B =
     * 33B > 31B, confirmed on-device: ble_gap_adv_set_fields returned
     * BLE_HS_EMSGSIZE=4 when all three were packed into one AD). Per OQ-2,
     * the name goes in the scan response instead. */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    ble_uuid128_t adv_uuid = s_svc_uuid;
    fields.uuids128 = &adv_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)BLE_OTA_ADV_DEVICE_NAME;
    rsp_fields.name_len = (uint8_t)strlen(BLE_OTA_ADV_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                            gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
        return;
    }
    s_advertising = true;
    ESP_LOGI(TAG, "FWUPG_ADV_START");
}

static void stop_advertising(const char *reason)
{
    if (s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
        ESP_LOGI(TAG, "FWUPG_ADV_STOP reason=%s", reason);
    }
    if (s_readv_timer != NULL) {
        esp_timer_stop(s_readv_timer);
    }
}

/* ---- GAP events ---- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (s_connected) {
                /* FR-206: only one concurrent client; belt-and-braces on
                 * top of CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1. */
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            s_peer_bonded = false;
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                snprintf(s_peer_addr_str, sizeof(s_peer_addr_str),
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                         desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                         desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            }
            ESP_LOGI(TAG, "FWUPG_CONNECT peer=%s", s_peer_addr_str);
            s_advertising = false; /* undirected adv auto-stops on connect */
            if (s_readv_timer != NULL) {
                esp_timer_stop(s_readv_timer);
            }
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "FWUPG_DISCONNECT reason=%d", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_peer_bonded = false;
        ble_ota_proto_on_disconnect();
        if (s_running) {
            start_advertising(); /* OQ-9: auto-resume, bounded */
            if (s_readv_timer != NULL) {
                esp_timer_start_once(s_readv_timer,
                                      (uint64_t)CONFIG_BLE_OTA_READVERTISE_WINDOW_S * 1000000ULL);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "FWUPG_BOND_FAIL reason=%d", event->enc_change.status);
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            if (desc.sec_state.encrypted && desc.sec_state.bonded) {
                s_peer_bonded = true;
                ESP_LOGI(TAG, "FWUPG_BOND_OK peer=%s", s_peer_addr_str);
                ble_ota_proto_on_connect();
            } else {
                /* FR-306: link degraded mid-session -- abort any transfer. */
                ble_ota_proto_on_disconnect();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "CCCD subscribe cur_notify=%d", event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

/* ---- host lifecycle callbacks ---- */

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
        return;
    }
    start_advertising();
}

static void host_task_fn(void *param)
{
    (void)param;
    nimble_port_run(); /* blocks until nimble_port_stop() */
    if (s_host_task_done != NULL) {
        xSemaphoreGive(s_host_task_done);
    }
    vTaskDelete(NULL);
}

/* ---- public API ---- */

esp_err_t ble_ota_transport_start(void)
{
    if (s_running) {
        return ESP_ERR_BLE_OTA_ALREADY_RUNNING;
    }
    if (ble_ota_health_is_pending_verify()) {
        /* Finding F-2 (tech_spec.md §6.4(1)): refuse without raising the
         * radio at all. cmd_fwupgrade.c formats the operator-facing text
         * using ble_ota_health_remaining_deadline_s(). */
        return ESP_ERR_BLE_OTA_PENDING_VERIFY;
    }

    if (!s_mem_released_once) {
        /* One-time, BLE-only build: release classic-BT memory we never
         * need. This is NOT the BLE mode FR-105 protects -- releasing
         * classic-BT memory on a BLE-only NimBLE build is standard
         * practice and must only be done once, ever (not per start/stop
         * cycle), hence the one-shot guard. */
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        s_mem_released_once = true;
    }

    /* nimble_port_init() (esp-idf components/bt/host/nimble/nimble/porting/
     * nimble/src/nimble_port.c) already performs the full controller
     * bring-up itself on this target -- esp_bt_controller_mem_release(),
     * esp_bt_controller_init(), esp_bt_controller_enable(), and (via
     * esp_nimble_init()) esp_nimble_hci_init() -- all internally, guarded
     * by CONFIG_BT_CONTROLLER_ENABLED. Calling any of those manually before
     * nimble_port_init() left the controller already in
     * ESP_BT_CONTROLLER_STATUS_ENABLED, so nimble_port_init()'s own
     * internal esp_bt_controller_init() call then failed its
     * btdm_controller_status == IDLE precondition
     * (components/bt/controller/esp32/bt.c) with ESP_ERR_INVALID_STATE --
     * logged under IDF's own "BLE_INIT" tag, not this component's. Do not
     * duplicate that bring-up here; nimble_port_deinit() undoes all of it
     * symmetrically on the way down. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* FR-303, OQ-3: Just Works -- single localised parameter block. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_OTA_ADV_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service registration failed: rc=%d", rc);
        /* nimble_port_deinit() unwinds esp_nimble_init()/esp_nimble_hci_init()
         * plus esp_bt_controller_disable()+deinit() internally -- see
         * ble_ota_transport_start()'s bring-up comment above. */
        nimble_port_deinit();
        return ESP_FAIL;
    }

#if CONFIG_BLE_OTA_ERASE_BONDS_ON_START
    ble_store_clear();
#endif

    err = ble_ota_proto_attach(notify_fn_impl, link_ctl_fn_impl);
    if (err != ESP_OK) {
        /* nimble_port_deinit() unwinds esp_nimble_init()/esp_nimble_hci_init()
         * plus esp_bt_controller_disable()+deinit() internally -- see
         * ble_ota_transport_start()'s bring-up comment above. */
        nimble_port_deinit();
        return err;
    }

    s_notify_q = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(notify_item_t));
    if (s_notify_q == NULL) {
        /* nimble_port_deinit() unwinds esp_nimble_init()/esp_nimble_hci_init()
         * plus esp_bt_controller_disable()+deinit() internally -- see
         * ble_ota_transport_start()'s bring-up comment above. */
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(notify_task_fn, "ble_ota_notify", NOTIFY_TASK_STACK, NULL,
                                NOTIFY_TASK_PRIO, &s_notify_task);
    if (ok != pdPASS) {
        vQueueDelete(s_notify_q);
        s_notify_q = NULL;
        /* nimble_port_deinit() unwinds esp_nimble_init()/esp_nimble_hci_init()
         * plus esp_bt_controller_disable()+deinit() internally -- see
         * ble_ota_transport_start()'s bring-up comment above. */
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    if (s_host_task_done == NULL) {
        s_host_task_done = xSemaphoreCreateBinary();
    }
    if (s_readv_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = readv_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ble_ota_readv",
        };
        esp_timer_create(&targs, &s_readv_timer);
    }

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_running = true;

    nimble_port_freertos_init(host_task_fn);

    ESP_LOGI(TAG, "FWUPG_BLE_UP");
    return ESP_OK;
}

esp_err_t ble_ota_transport_stop(void)
{
    if (!s_running) {
        return ESP_ERR_BLE_OTA_NOT_RUNNING;
    }

    stop_advertising("fwupgrade_stop");
    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_ota_proto_abort("fwupgrade_stop"); /* FR-703 */

    if (s_notify_task != NULL) {
        vTaskDelete(s_notify_task);
        s_notify_task = NULL;
    }
    if (s_notify_q != NULL) {
        vQueueDelete(s_notify_q);
        s_notify_q = NULL;
    }

    nimble_port_stop();
    if (s_host_task_done != NULL) {
        xSemaphoreTake(s_host_task_done, pdMS_TO_TICKS(2000));
    }

    /* Symmetric with ble_ota_transport_start(): nimble_port_deinit() alone
     * tears down the host, esp_nimble_hci, and the BT controller (disable
     * + deinit) -- see the bring-up comment in ble_ota_transport_start(). */
    nimble_port_deinit();

    s_running = false;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "FWUPG_BLE_DOWN");
    return ESP_OK;
}

bool ble_ota_transport_is_running(void)
{
    return s_running;
}

esp_err_t ble_ota_transport_get_status(ble_ota_transport_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->advertising = s_advertising;
    out->connected = s_connected;
    out->bonded = s_peer_bonded;
    if (s_connected) {
        strncpy(out->peer_addr_str, s_peer_addr_str, sizeof(out->peer_addr_str) - 1);
        out->peer_addr_str[sizeof(out->peer_addr_str) - 1] = '\0';
    } else {
        out->peer_addr_str[0] = '\0';
    }
    return ESP_OK;
}
