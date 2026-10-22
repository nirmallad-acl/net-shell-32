/**
 * @file ble_ota_transport_nimble.c
 * @brief L1: OTA client of ble_host -- the ONLY translation unit in this component that includes a NimBLE header
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
 * Implements FR-201..206 (GATT service), FR-301..306 (runtime security check)
 * and OQ-9 (bounded re-advertise window), now as a CLIENT of the ble_host
 * component (NETSHELL32-5, tech_spec.md 2.4): the NimBLE host, GAP events,
 * advertising and security configuration moved to ble_host. This layer keeps the
 * OTA service definition (UUIDs, flags and permissions unchanged), delivers raw
 * RX frames downward to L2 via ble_ota_proto_rx() and registers its notify and
 * link-control callbacks via ble_ota_proto_attach().
 *
 * Added here: the software arm/disarm gate (REQ-FUNC-75), the OTA TX gate, the
 * once-per-generation on_connect (REQ-FUNC-77), the bounded re-advertise window
 * that DISARMS instead of stopping the radio (REQ-FUNC-79) and the reboot guard
 * ops (REQ-FUNC-31). The decision logic itself lives in ble_ota_gate.c (pure).
 */
#include "ble_ota_transport.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "ble_host.h"
#include "ble_host_client.h"
#include "ble_ota_flash.h"
#include "ble_ota_gate.h"
#include "ble_ota_health.h"
#include "ble_ota_internal.h"
#include "ble_ota_proto.h"
#include "ble_ota_proto_defs.h"

#if CONFIG_BLE_OTA_ERASE_BONDS_ON_START
#error "CONFIG_BLE_OTA_ERASE_BONDS_ON_START must stay n: the bond store is shared with the BLE CLI (REQ-BLD-04)"
#endif

static const char *TAG = BLE_OTA_TAG_BLE;

#define NOTIFY_QUEUE_DEPTH  4
#define NOTIFY_TASK_STACK   3072
#define NOTIFY_TASK_PRIO    5
#define NOTIFY_RETRY_COUNT  3
#define NOTIFY_RETRY_MS     100
#define BLE_OTA_STUCK_RETRY_MS 5000u  /* janitor retry period of a DRAIN_STUCK drain (R3-06) */
#define RX_REJECT_LOG_GAP_US   1000000LL /* FWUPG_RX_REJECT is rate-limited to 1 line/s */

#define RX_BUF_MAX (2u + CONFIG_BLE_OTA_MAX_CHUNK_SIZE)

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(BLE_OTA_SVC_UUID128_BYTES_LE);
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(BLE_OTA_RX_CHR_UUID128_BYTES_LE);
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(BLE_OTA_TX_CHR_UUID128_BYTES_LE);
static const uint8_t       s_adv_uuid[16] = { BLE_OTA_SVC_UUID128_BYTES_LE };

static uint16_t s_tx_val_handle;
static uint16_t s_rx_val_handle;

typedef struct {
    uint8_t  len; /* 0 = control "kick" item, never sent on air */
    uint8_t  data[BLE_OTA_NOTIFY_MAX_LEN];
    uint32_t gen;
    uint32_t arm_epoch;
} notify_item_t;

/* ---- registration state (ADV-OTA-11) ---- */
static bool      s_reg_attempted;
static esp_err_t s_reg_result = ESP_FAIL;
static bool      s_ota_registered;
static ble_host_client_id_t s_client_id;

/* ---- notify / control task ---- */
static StaticQueue_t s_notify_q_buf;
static uint8_t       s_notify_q_store[NOTIFY_QUEUE_DEPTH * sizeof(notify_item_t)];
static QueueHandle_t s_notify_q;
static TaskHandle_t  s_notify_task;

/* ---- host-task-only / cross-task state ---- */
static _Atomic uint32_t s_ota_tx_subscribed;
static _Atomic uint32_t s_expire_pending; /* epoch of an expired window, 0 = none */
static _Atomic uint32_t s_readv_epoch = 1;
static uint32_t         s_timer_epoch;     /* written before the window timer is started */
static bool             s_prev_secure;     /* host task only */
static esp_timer_handle_t s_readv_timer;
static uint32_t         s_stuck_retry_ms;  /* ctl-task private */
static int64_t          s_rx_reject_log_us; /* host task only */
static uint8_t          s_rx_buf[RX_BUF_MAX]; /* host task only: no 514 B array on the host stack */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t readv_bump(void)
{
    uint32_t e = atomic_fetch_add(&s_readv_epoch, 1u) + 1u;
    if (e == 0u) {
        e = atomic_fetch_add(&s_readv_epoch, 1u) + 1u;
    }
    return e;
}

/* ---- RX access callback (host task; the waiver of REQ-PERF-03 applies, 2.4.9) ---- */

static int gatt_access_rx(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* REQ-FUNC-75: the protocol receives zero RX calls unless armed. */
    ota_rx_verdict_t verdict = ble_ota_gate_rx_decision();
    if (verdict != OTA_RX_FORWARD) {
        int64_t now = esp_timer_get_time();
        if (now - s_rx_reject_log_us >= RX_REJECT_LOG_GAP_US) {
            s_rx_reject_log_us = now;
            ESP_LOGW(TAG, "FWUPG_RX_REJECT reason=%s",
                     (verdict == OTA_RX_REJECT_REBOOT_PENDING) ? "reboot_pending" : "not_armed");
        }
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED; /* 0x03, Gate 2 G2-04 */
    }

    const int64_t t0 = esp_timer_get_time();
    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    if (total_len == 0 || total_len > RX_BUF_MAX) {
        /* FR-302: rejected before any field is read -- no partial processing,
         * no state change, no flash access. */
        ESP_LOGW(TAG, "FWUPG_RX_REJECT reason=oversized_frame len=%u", (unsigned)total_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf), &out_len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    bool secure = ble_host_link_secure(conn_handle);
    ble_ota_proto_rx(secure, s_rx_buf, out_len);
    if (secure) {
        /* Only an ACCEPTED (secure) write engages the OTA client; an unsecured frame is
         * dropped by the protocol and must not influence the link-release policy. */
        ble_host_note_engaged(s_client_id, ble_host_current_gen());
    }

    int64_t dt = esp_timer_get_time() - t0;
    ble_host_note_ota_rx_us((dt > 0) ? (uint32_t)dt : 0u);
    return 0;
}

static int gatt_access_tx(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* Notify-only: no direct read/write is ever legal on this attribute. */
    return BLE_ATT_ERR_UNLIKELY;
}

/* Service definition: UUIDs, flags and permissions are unchanged from NETSHELL32-2
 * (REQ-FUNC-74). The RX characteristic only gained a val_handle pointer, used for
 * the FWUPG_HANDLES log (invariant I-1 check, V-04). */
static const struct ble_gatt_chr_def s_gatt_chrs[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = gatt_access_rx,
        .val_handle = &s_rx_val_handle,
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

/* ---- upward callbacks registered with L2 (tech_spec.md 3.3) ---- */

static esp_err_t notify_fn_impl(const uint8_t *frame, size_t len)
{
    if (s_notify_q == NULL || frame == NULL || len == 0 || len > BLE_OTA_NOTIFY_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    notify_item_t item;
    memset(&item, 0, sizeof(item));
    item.len = (uint8_t)len;
    memcpy(item.data, frame, len);
    item.gen = ble_host_current_gen();     /* tagged at enqueue (ADV-NEW-05) */
    item.arm_epoch = ble_ota_gate_epoch();
    if (xQueueSend(s_notify_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "notify queue full; dropping frame op=0x%02x", frame[0]);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void link_ctl_fn_impl(ble_ota_link_ctl_cmd_t cmd)
{
    uint16_t conn = 0;
    if (cmd == BLE_OTA_LINK_CTL_DISCONNECT && ble_host_get_link(&conn, NULL, NULL, NULL) == ESP_OK) {
        (void)ble_host_terminate(conn);
    }
}

/* ---- arm / disarm (REQ-FUNC-75..77, 79) ---- */

static void readv_window_stop(void)
{
    if (s_readv_timer != NULL) {
        (void)esp_timer_stop(s_readv_timer);
    }
}

static void readv_window_start(void)
{
    if (s_readv_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_readv_timer);
    s_timer_epoch = atomic_load(&s_readv_epoch); /* before the timer can fire */
    (void)esp_timer_start_once(s_readv_timer, (uint64_t)CONFIG_BLE_OTA_READVERTISE_WINDOW_S * 1000000ULL);
}

/* on_connect exactly once per generation, when it can actually be delivered
 * (armed, secure and OTA TX subscribed). Host task only. */
static void maybe_on_connect(void)
{
    uint16_t conn = 0;
    uint32_t gen = 0;
    bool secure = false;
    if (ble_host_get_link(&conn, &gen, &secure, NULL) != ESP_OK) {
        return;
    }
    if (ble_ota_gate_on_connect_due(secure, atomic_load(&s_ota_tx_subscribed) != 0u, gen)) {
        ble_ota_proto_on_connect();
    }
}

/* Exec function (host task): CAS first, side effects only if it was won. */
static int32_t arm_fn(uintptr_t arg)
{
    (void)arg;
    if (ble_ota_gate_arm_commit() != OTA_EXEC_CLAIMED) {
        return OTA_EXEC_NOT_CLAIMED;
    }
    (void)ble_host_set_adv_primary(s_client_id, true);
    readv_window_stop();
    (void)readv_bump();
    bool peer = (ble_host_get_link(NULL, NULL, NULL, NULL) == ESP_OK);
    ESP_LOGI(TAG, "FWUPG_ARM peer_connected=%d", peer ? 1 : 0);
    maybe_on_connect();
    return OTA_EXEC_CLAIMED;
}

/* Exec function (host task). arg = 0: "fwupgrade stop"; arg = expiry epoch:
 * re-advertise window expiry (valid only with no link and an unchanged epoch). */
static int32_t disarm_fn(uintptr_t arg)
{
    const uint32_t e = (uint32_t)arg;
    const char *reason = (e != 0u) ? "readvertise_window_expired" : "fwupgrade_stop";
    bool link_up = (ble_host_get_link(NULL, NULL, NULL, NULL) == ESP_OK);
    int32_t r = ble_ota_gate_disarm_commit(reason, e, link_up, atomic_load(&s_readv_epoch), now_ms());
    if (r != OTA_EXEC_CLAIMED) {
        return r; /* NOT_CLAIMED or STALE: no side effect at all */
    }
    /* After the CAS: the gate is closed, now the payload, the timer and the link. */
    (void)ble_host_set_adv_primary(s_client_id, false);
    readv_window_stop();
    (void)readv_bump();
    (void)ble_host_release_link(s_client_id, ble_host_current_gen()); /* terminates an OTA-only link */
    (void)ble_ota_proto_abort(reason);
    return OTA_EXEC_CLAIMED;
}

/* Drain claimant only (the DISARMING owner). Returns true if DISARMED was reached. */
static bool drain_run(bool recovered)
{
    const char *reason = ble_ota_gate_disarm_reason();
    ble_ota_gate_clear_aborted_at();
    (void)ble_ota_proto_abort(reason); /* idempotent; covers a mutex timeout inside disarm_fn */
    if (ble_ota_proto_wait_idle(CONFIG_BLE_OTA_DRAIN_TIMEOUT_MS)) {
        ble_ota_flash_stop(); /* INV-A4: only the drain claimant calls this */
        ble_ota_gate_drain_done(true);
        ESP_LOGI(TAG, "FWUPG_DISARM reason=%s%s", reason, recovered ? " drain=recovered" : "");
        return true;
    }
    ble_ota_gate_drain_done(false); /* worker deliberately left running */
    ESP_LOGW(TAG, "FWUPG_DISARM reason=%s drain=stuck", reason);
    return false;
}

typedef enum {
    DD_DONE = 0,
    DD_STUCK,
    DD_NOT_CLAIMED,
    DD_HOST_TIMEOUT,
    DD_BUSY,
} dd_outcome_t;

/* The one disarm procedure shared by stop and the window expiry (tech_spec 2.4.3). */
static dd_outcome_t ota_disarm_and_drain(uint32_t expiry_epoch)
{
    int32_t res = OTA_EXEC_NOT_CLAIMED;
    esp_err_t rc = ble_host_exec(disarm_fn, (uintptr_t)expiry_epoch, CONFIG_BLE_HOST_EXEC_TIMEOUT_MS, &res);

    if (rc == ESP_OK) {
        if (res != OTA_EXEC_CLAIMED) {
            return DD_NOT_CLAIMED; /* nothing was done by this request */
        }
        if (!ble_ota_gate_claim_drain(OTA_ARM_ABORTED)) {
            return DD_BUSY;
        }
        return drain_run(false) ? DD_DONE : DD_STUCK;
    }
    if (rc == ESP_ERR_BLE_HOST_EXEC_INDETERMINATE) {
        /* Unknown outcome and the fn may still run: try the claim once. If disarm_fn
         * completes later the state is ABORTED and the janitor claims it within ~2 s. */
        if (ble_ota_gate_claim_drain(OTA_ARM_ABORTED)) {
            return drain_run(false) ? DD_DONE : DD_STUCK;
        }
        return DD_BUSY;
    }
    return DD_HOST_TIMEOUT; /* TIMEOUT / NOT_UP / WRONG_TASK: fn never ran, state unchanged */
}

ble_ota_arm_result_t ble_ota_transport_arm(bool *peer_connected, esp_err_t *flash_err)
{
    if (peer_connected != NULL) {
        *peer_connected = false;
    }
    if (ble_host_is_starting()) {
        return OTA_ARM_HOST_STARTING;
    }
    if (!ble_host_is_up()) {
        return OTA_ARM_HOST_UNAVAILABLE;
    }
    if (!s_ota_registered) {
        return OTA_ARM_NOT_REGISTERED;
    }
    if (ble_ota_health_is_pending_verify()) {
        return OTA_ARM_PENDING_VERIFY;
    }

    /* Claim DISARMED -> ARMING. A leftover ABORTED/DRAIN_STUCK is drained first. */
    bool claimed = false;
    for (int pass = 0; pass < 2 && !claimed; pass++) {
        if (ble_ota_gate_try_arming()) {
            claimed = true;
            break;
        }
        ota_arm_state_t st = ble_ota_gate_state();
        switch (st) {
        case OTA_ARM_ARMED:
            return OTA_ARM_ALREADY_RUNNING;
        case OTA_ARM_ABORTED:
        case OTA_ARM_DRAIN_STUCK:
            if (!ble_ota_gate_claim_drain(st)) {
                return OTA_ARM_BUSY;
            }
            if (!drain_run(st == OTA_ARM_DRAIN_STUCK)) {
                return OTA_ARM_RES_DRAIN_STUCK;
            }
            break; /* now DISARMED: take the second pass */
        case OTA_ARM_DISARMED:
            break; /* lost a race in the meantime: retry */
        default:
            return OTA_ARM_BUSY; /* ARMING / DISARMING */
        }
    }
    if (!claimed) {
        return OTA_ARM_BUSY;
    }

    esp_err_t ferr = ble_ota_flash_start();
    if (ferr != ESP_OK) {
        (void)ble_ota_gate_cancel_arming();
        if (flash_err != NULL) {
            *flash_err = ferr;
        }
        return OTA_ARM_FLASH_FAIL;
    }

    int32_t res = OTA_EXEC_NOT_CLAIMED;
    esp_err_t rc = ble_host_exec(arm_fn, 0, CONFIG_BLE_HOST_EXEC_TIMEOUT_MS, &res);
    if (rc == ESP_OK && res == OTA_EXEC_CLAIMED) {
        if (peer_connected != NULL) {
            *peer_connected = (ble_host_get_link(NULL, NULL, NULL, NULL) == ESP_OK);
        }
        return OTA_ARM_OK;
    }
    /* Any other outcome: the CAS ARMING -> DISARMED decides who owns the cleanup. */
    if (ble_ota_gate_cancel_arming()) {
        ble_ota_flash_stop();
        return OTA_ARM_HOST_TIMEOUT;
    }
    if (ble_ota_gate_state() == OTA_ARM_ARMED) {
        /* A late arm_fn won after an INDETERMINATE exec: it is armed. */
        if (peer_connected != NULL) {
            *peer_connected = (ble_host_get_link(NULL, NULL, NULL, NULL) == ESP_OK);
        }
        return OTA_ARM_OK;
    }
    return OTA_ARM_INTERNAL; /* unreachable: only start and arm_fn write from ARMING */
}

ble_ota_disarm_result_t ble_ota_transport_disarm(void)
{
    ble_ota_disarm_result_t out = { OTA_DISARM_OK, "fwupgrade_stop" };
    ota_arm_state_t st = ble_ota_gate_state();

    switch (st) {
    case OTA_ARM_DISARMED:
        out.kind = OTA_DISARM_NOT_RUNNING;
        return out;
    case OTA_ARM_ARMING:
    case OTA_ARM_DISARMING:
        out.kind = OTA_DISARM_BUSY;
        return out;
    case OTA_ARM_ABORTED:
    case OTA_ARM_DRAIN_STUCK:
        if (!ble_ota_gate_claim_drain(st)) {
            out.kind = OTA_DISARM_BUSY;
            return out;
        }
        out.kind = drain_run(st == OTA_ARM_DRAIN_STUCK) ? OTA_DISARM_OK : OTA_DISARM_DRAIN_STUCK;
        return out;
    default:
        break;
    }

    switch (ota_disarm_and_drain(0)) {
    case DD_DONE:
        out.kind = OTA_DISARM_OK;
        break;
    case DD_STUCK:
        out.kind = OTA_DISARM_DRAIN_STUCK;
        break;
    case DD_HOST_TIMEOUT:
        out.kind = OTA_DISARM_HOST_TIMEOUT;
        break;
    case DD_NOT_CLAIMED:
    case DD_BUSY:
    default:
        /* Another disarm (the expiry) got there first: report what it left behind. */
        out.reason = ble_ota_gate_disarm_reason();
        out.kind = (ble_ota_gate_state() == OTA_ARM_DISARMED) ? OTA_DISARM_NOT_RUNNING
                 : (ble_ota_gate_state() == OTA_ARM_DRAIN_STUCK) ? OTA_DISARM_DRAIN_STUCK
                                                                : OTA_DISARM_IN_PROGRESS;
        break;
    }
    return out;
}

/* ---- notify / control task (tech_spec.md 2.4.5) ---- */

static void gate_and_send(const notify_item_t *item)
{
    if (item->len == 0u) {
        return; /* kick item: wake-up only */
    }
    uint16_t conn = 0;
    uint32_t gen = 0;
    bool link_secure_snap = false;
    bool link = (ble_host_get_link(&conn, &gen, &link_secure_snap, NULL) == ESP_OK);

    ota_tx_in_t in = {
        .item_gen = item->gen,
        .item_epoch = item->arm_epoch,
        .link_present = link,
        .cur_gen = link ? gen : 0u,
        .subscribed = (atomic_load(&s_ota_tx_subscribed) != 0u),
        .secure = link && ble_host_link_secure(conn), /* live check */
    };
    ota_tx_verdict_t v = ble_ota_gate_tx_check(&in);
    if (v != OTA_TX_OK) {
        ESP_LOGD(TAG, "FWUPG_TX_SUPPRESS op=0x%02x reason=%s", item->data[0], ble_ota_gate_tx_reason(v));
        return;
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < NOTIFY_RETRY_COUNT; attempt++) {
        err = ble_host_notify(conn, s_tx_val_handle, item->data, item->len, item->gen);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify send failed after retries: %s op=0x%02x", esp_err_to_name(err), item->data[0]);
    }
}

static void notify_task_fn(void *arg)
{
    (void)arg;
    notify_item_t item;
    for (;;) {
        bool got = (xQueueReceive(s_notify_q, &item, pdMS_TO_TICKS(1000)) == pdTRUE); /* bounded */

        uint32_t e = atomic_exchange(&s_expire_pending, 0u);
        if (e != 0u) {
            if (ota_disarm_and_drain(e) == DD_HOST_TIMEOUT) {
                uint32_t zero = 0u; /* retry at the next wake while the epoch is unchanged */
                (void)atomic_compare_exchange_strong(&s_expire_pending, &zero, e);
            }
        }

        const uint32_t now = now_ms();
        if (ble_ota_gate_janitor_aborted_due(now) && ble_ota_gate_claim_drain(OTA_ARM_ABORTED)) {
            (void)drain_run(false);
        }
        if (ble_ota_gate_state() == OTA_ARM_DRAIN_STUCK &&
            (uint32_t)(now - s_stuck_retry_ms) >= BLE_OTA_STUCK_RETRY_MS) {
            s_stuck_retry_ms = now;
            if (ble_ota_proto_wait_idle(0) && ble_ota_gate_claim_drain(OTA_ARM_DRAIN_STUCK)) {
                (void)drain_run(true); /* R3-06: drain=recovered */
            }
        }

        if (got) {
            gate_and_send(&item);
        }
    }
}

static void readv_timer_cb(void *arg)
{
    (void)arg;
    /* The flag is the source of truth; the kick is a best-effort wake-up. */
    atomic_store(&s_expire_pending, s_timer_epoch);
    if (s_notify_q != NULL) {
        notify_item_t kick;
        memset(&kick, 0, sizeof(kick));
        (void)xQueueSendToFront(s_notify_q, &kick, 0); /* never blocks; failure is harmless */
    }
}

/* ---- host events (host task) ---- */

static bool armed(void)
{
    return ble_ota_gate_state() == OTA_ARM_ARMED;
}

static void link_loss(void)
{
    /* Whenever not DISARMED (INV-A2): abort is idempotent; DISARMED has nothing to abort. */
    if (ble_ota_gate_link_loss_needs_abort()) {
        ble_ota_proto_on_disconnect();
    }
    atomic_store(&s_ota_tx_subscribed, 0u);
    s_prev_secure = false;
    if (armed()) {
        readv_window_start(); /* REQ-FUNC-79 */
    }
}

static void on_host_evt(const ble_host_evt_t *e, void *user)
{
    (void)user;
    switch (e->type) {
    case BLE_HOST_EVT_SYNC:
        ESP_LOGI(TAG, "FWUPG_HANDLES rx=%u tx=%u", (unsigned)s_rx_val_handle, (unsigned)s_tx_val_handle);
        break;

    case BLE_HOST_EVT_CONNECT:
        atomic_store(&s_ota_tx_subscribed, 0u);
        s_prev_secure = false;
        readv_window_stop();
        (void)readv_bump(); /* makes a pending expiry STALE (INV-A7) */
        ble_ota_gate_on_connect_reset();
        if (armed()) {
            ESP_LOGI(TAG, "FWUPG_CONNECT peer=%s", e->peer_addr);
        }
        break;

    case BLE_HOST_EVT_DISCONNECT:
        if (armed()) {
            ESP_LOGW(TAG, "FWUPG_DISCONNECT reason=%d", e->status);
        }
        link_loss();
        break;

    case BLE_HOST_EVT_HOST_RESET:
        link_loss();
        break;

    case BLE_HOST_EVT_ENC_CHANGE:
        if (e->secure) {
            s_prev_secure = true;
            if (armed()) {
                ESP_LOGI(TAG, "FWUPG_BOND_OK peer=%s", e->peer_addr);
            }
            maybe_on_connect();
        } else {
            if (armed() && e->status != 0) {
                ESP_LOGW(TAG, "FWUPG_BOND_FAIL reason=%d", e->status);
            }
            if (s_prev_secure) {
                /* FR-306: security lost mid-session. A generation bump ends the old
                 * subscription as well. */
                s_prev_secure = false;
                atomic_store(&s_ota_tx_subscribed, 0u);
                if (ble_ota_gate_link_loss_needs_abort()) {
                    ble_ota_proto_on_disconnect();
                }
            }
        }
        break;

    case BLE_HOST_EVT_SUBSCRIBE:
        if (e->attr_handle != s_tx_val_handle) {
            break; /* every other handle (Service Changed, CLI TX) is ignored (ADV-BLE-07) */
        }
        if (e->notify) {
            atomic_store(&s_ota_tx_subscribed, 1u); /* WRITE or RESTORE */
            if (armed() && e->secure && e->sub_reason == BLE_HOST_SUB_WRITE) {
                ble_host_note_engaged(s_client_id, e->gen);
            }
            maybe_on_connect();
        } else {
            atomic_store(&s_ota_tx_subscribed, 0u);
        }
        break;

    default:
        break;
    }
}

/* ---- reboot guard ops (used by ble_ota.c) ---- */

esp_err_t ble_ota_transport_register(void)
{
    if (s_reg_attempted) {
        return s_reg_result;
    }
    s_reg_attempted = true;

    esp_err_t err = ble_ota_proto_attach(notify_fn_impl, link_ctl_fn_impl);
    if (err == ESP_OK && s_notify_q == NULL) {
        s_notify_q = xQueueCreateStatic(NOTIFY_QUEUE_DEPTH, sizeof(notify_item_t), s_notify_q_store,
                                        &s_notify_q_buf);
        if (s_notify_q == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK && s_notify_task == NULL) {
        /* Created once at boot and never deleted (a mid-notify delete was a latent hazard). */
        if (xTaskCreate(notify_task_fn, "ble_ota_notify", NOTIFY_TASK_STACK, NULL, NOTIFY_TASK_PRIO,
                        &s_notify_task) != pdPASS) {
            s_notify_task = NULL;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK && s_readv_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = readv_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ble_ota_readv",
        };
        err = esp_timer_create(&targs, &s_readv_timer);
    }
    if (err == ESP_OK) {
        const ble_host_client_cfg_t cfg = {
            .name = "ota",
            .svcs = s_gatt_svcs,
            .adv_uuid128 = s_adv_uuid,
            .adv_policy = BLE_HOST_ADV_WHEN_PRIMARY,
            .default_primary = false,
            .cb = on_host_evt,
            .user = NULL,
            .waive_timing = true, /* REQ-PERF-03 waiver, tech_spec.md 2.4.9 */
        };
        err = ble_host_register_client(&cfg, &s_client_id);
    }
    s_ota_registered = (err == ESP_OK);
    s_reg_result = err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FWUPG_REGISTER_FAIL err=%s", esp_err_to_name(err));
    }
    return err;
}

bool ble_ota_transport_is_registered(void)
{
    return s_ota_registered;
}

esp_err_t ble_ota_transport_registration_result(void)
{
    return s_reg_result;
}

esp_err_t ble_ota_transport_get_status(ble_ota_transport_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->host_up = ble_host_is_up();
    out->host_starting = ble_host_is_starting();
    out->registered = s_ota_registered;
    out->arm_state = ble_ota_gate_state();
    out->adv_mode_armed = (out->arm_state == OTA_ARM_ARMED);
    out->advertising = ble_host_is_advertising();
    bool secure = false;
    char peer[18];
    peer[0] = '\0';
    if (ble_host_get_link(NULL, NULL, &secure, peer) == ESP_OK) {
        out->connected = true;
        out->bonded = secure;
        memcpy(out->peer_addr_str, peer, sizeof(out->peer_addr_str));
        out->peer_addr_str[sizeof(out->peer_addr_str) - 1] = '\0';
    }
    out->cli_engaged = (s_ota_registered && (ble_host_engaged_mask() & ~(1u << s_client_id)) != 0u);
    return ESP_OK;
}
