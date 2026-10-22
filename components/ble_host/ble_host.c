/**
 * @file ble_host.c
 * @brief Neutral NimBLE owner: bring-up, GAP events, advertising, exec, link helpers
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
 * NETSHELL32-5, tech_spec.md section 2.3. REQ-FUNC-70..73, 78, 80, 64, 65,
 * REQ-REL-03, REQ-INT-05, REQ-INT-08, REQ-INT-10.
 *
 * This file never includes a client header (ble_ota, ble_cli) and has no
 * dependency on one. All GAP events and GATT callbacks run on the NimBLE host
 * task; everything that must be atomic with respect to them (advertising
 * payload switches, arm/disarm, the reboot commit) runs there through
 * ble_host_exec().
 */
#include "ble_host.h"
#include "ble_host_client.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "adv_payload.h"
#include "host_exec.h"
#include "host_registry.h"

_Static_assert(sizeof(CONFIG_BLE_HOST_DEVICE_NAME) - 1 <= 10,
               "BLE_HOST_DEVICE_NAME max 10 chars: armed scan response = 2+name + 18 must be <= 31 B");

static const char *TAG = "ble_host";

#define SYNC_WATCHDOG_US  (5ULL * 1000000ULL)
#define SEC_RETRY_DELAY_US (2ULL * 1000000ULL)
#define GATT_CRC_NVS_NS   "ble_host"
#define GATT_CRC_NVS_KEY  "gatt_crc"

enum {
    HS_UNINIT = 0,
    HS_INIT,
    HS_STARTING,
    HS_UP,
    HS_FAILED,
};

/* ---- state shared between tasks ---- */
static _Atomic uint32_t s_state = HS_UNINIT;
static _Atomic uint32_t s_gen_pub = 1;       /* mirror of the registry generation */
static _Atomic uint32_t s_engaged_pub;       /* mirror of the engagement mask */
static _Atomic uint32_t s_adv_active_pub;    /* mirror of s_advertising */
static _Atomic uint32_t s_host_pending;      /* HOST_PEND_* bits */
static _Atomic uint32_t s_deadline_gen;      /* generation the security deadline was armed for */
static _Atomic uint32_t s_cb_max_us;
static _Atomic uint32_t s_ota_rx_max_us;

/* ---- host-task-only state (written only on the host task) ---- */
static host_reg_t s_reg;
static host_exec_t s_exec;
static uint8_t  s_own_addr_type;
static bool     s_advertising;
static bool     s_synced_once;
static bool     s_adv_fail_logged;
static uint8_t  s_adv_retry_idx;
static uint8_t  s_sec_retries_left;
static uint32_t s_sec_retry_gen;
static uint8_t  s_adv_buf[ADV_PAYLOAD_MAX];
static uint8_t  s_rsp_buf[ADV_PAYLOAD_MAX];
static bool     s_mem_released_once;

static TaskHandle_t s_host_task;

/* Link record: written on the host task under s_link_mux, read by any task. */
static portMUX_TYPE s_link_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool     up;
    uint16_t conn;
    uint32_t gen;
    bool     secure;
    uint16_t mtu;
    char     peer[18];
} s_link;

/* ---- exec / kick plumbing ---- */
static StaticSemaphore_t s_exec_mtx_buf;
static StaticSemaphore_t s_exec_sem_buf;
static SemaphoreHandle_t s_exec_mtx;
static SemaphoreHandle_t s_exec_sem;
static struct ble_npl_event s_exec_ev;
static struct ble_npl_event s_kick_ev;

static esp_timer_handle_t s_sync_wd_timer;
static esp_timer_handle_t s_adv_retry_timer;
static esp_timer_handle_t s_sec_deadline_timer;
static esp_timer_handle_t s_sec_retry_timer;

static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static void host_pending_run(void);
static void adv_start_now(void);

static TickType_t ms_to_ticks_up(uint32_t ms)
{
    if (ms == 0) {
        return 0;
    }
    TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0) ? 1 : t;
}

static int64_t now_us_fn(void)
{
    return esp_timer_get_time();
}

/* ---- host_exec ops (FreeRTOS / NimBLE glue) ---- */

static int64_t ex_now_us(void *c)
{
    (void)c;
    return esp_timer_get_time();
}

static bool ex_mtx_take(void *c, uint32_t ms)
{
    (void)c;
    return xSemaphoreTake(s_exec_mtx, ms_to_ticks_up(ms)) == pdTRUE;
}

static void ex_mtx_give(void *c)
{
    (void)c;
    (void)xSemaphoreGive(s_exec_mtx);
}

static bool ex_sem_take(void *c, uint32_t ms)
{
    (void)c;
    return xSemaphoreTake(s_exec_sem, ms_to_ticks_up(ms)) == pdTRUE;
}

static void ex_sem_give(void *c)
{
    (void)c;
    (void)xSemaphoreGive(s_exec_sem);
}

static bool ex_post(void *c)
{
    (void)c;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_exec_ev);
    return true;
}

static bool ex_host_up(void *c)
{
    (void)c;
    return atomic_load(&s_state) == HS_UP;
}

static bool ex_on_host_task(void *c)
{
    (void)c;
    return s_host_task != NULL && xTaskGetCurrentTaskHandle() == s_host_task;
}

static void ex_log_stuck(void *c, uint32_t seq)
{
    (void)c;
    ESP_LOGE(TAG, "BLECLI_EXEC_STUCK seq=%" PRIu32, seq);
}

static const host_exec_ops_t s_exec_ops = {
    .ctx = NULL,
    .now_us = ex_now_us,
    .mtx_take = ex_mtx_take,
    .mtx_give = ex_mtx_give,
    .sem_take = ex_sem_take,
    .sem_give = ex_sem_give,
    .post = ex_post,
    .host_up = ex_host_up,
    .on_host_task = ex_on_host_task,
    .log_stuck = ex_log_stuck,
};

static void exec_ev_fn(struct ble_npl_event *ev)
{
    (void)ev;
    host_exec_run(&s_exec, &s_exec_ops);
}

static void kick_ev_fn(struct ble_npl_event *ev)
{
    (void)ev;
    host_pending_run();
}

/* Only the esp_timer task posts s_kick_ev (single poster, tech_spec 2.3.5). */
static void post_kick(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_kick_ev);
}

static void adv_retry_timer_cb(void *arg)
{
    (void)arg;
    host_pending_set(&s_host_pending, HOST_PEND_ADV_RETRY);
    post_kick();
}

static void sec_deadline_timer_cb(void *arg)
{
    (void)arg;
    host_pending_set(&s_host_pending, HOST_PEND_SEC_DEADLINE);
    post_kick();
}

static void sec_retry_timer_cb(void *arg)
{
    (void)arg;
    host_pending_set(&s_host_pending, HOST_PEND_SEC_RETRY);
    post_kick();
}

static void sync_wd_timer_cb(void *arg)
{
    (void)arg;
    if (atomic_load(&s_state) != HS_UP) {
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=sync_timeout");
    }
}

/* ---- link record helpers ---- */

static void link_set(uint16_t conn, uint32_t gen, bool secure, const char *peer)
{
    portENTER_CRITICAL(&s_link_mux);
    s_link.up = true;
    s_link.conn = conn;
    s_link.gen = gen;
    s_link.secure = secure;
    s_link.mtu = 23;
    memset(s_link.peer, 0, sizeof(s_link.peer));
    if (peer != NULL) {
        size_t n = strlen(peer);
        if (n > sizeof(s_link.peer) - 1) {
            n = sizeof(s_link.peer) - 1;
        }
        memcpy(s_link.peer, peer, n);
    }
    portEXIT_CRITICAL(&s_link_mux);
}

static void link_clear(void)
{
    portENTER_CRITICAL(&s_link_mux);
    s_link.up = false;
    s_link.conn = BLE_HS_CONN_HANDLE_NONE;
    s_link.gen = 0;
    s_link.secure = false;
    s_link.mtu = 23;
    s_link.peer[0] = '\0';
    portEXIT_CRITICAL(&s_link_mux);
}

static uint32_t gen_bump(void)
{
    uint32_t g = host_reg_next_gen(&s_reg);
    atomic_store(&s_gen_pub, g);
    host_reg_clear_engaged(&s_reg);
    atomic_store(&s_engaged_pub, 0u);
    return g;
}

static void dispatch(ble_host_evt_type_t type, uint16_t conn, bool secure, int status)
{
    ble_host_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.conn_handle = conn;
    evt.gen = host_reg_gen(&s_reg);
    evt.secure = secure;
    evt.status = status;
    portENTER_CRITICAL(&s_link_mux);
    memcpy(evt.peer_addr, s_link.peer, sizeof(evt.peer_addr));
    portEXIT_CRITICAL(&s_link_mux);
    evt.peer_addr[sizeof(evt.peer_addr) - 1] = '\0';

    uint32_t m = atomic_load(&s_cb_max_us);
    host_reg_dispatch_timed(&s_reg, &evt, now_us_fn, &m);
    atomic_store(&s_cb_max_us, m);
}

static void dispatch_evt(const ble_host_evt_t *evt)
{
    uint32_t m = atomic_load(&s_cb_max_us);
    host_reg_dispatch_timed(&s_reg, evt, now_us_fn, &m);
    atomic_store(&s_cb_max_us, m);
}

/* ---- security deadline and initiation (tech_spec 2.3.9) ---- */

static void sec_deadline_arm(uint32_t gen)
{
    atomic_store(&s_deadline_gen, gen);
    (void)esp_timer_stop(s_sec_deadline_timer);
    esp_err_t err = esp_timer_start_once(s_sec_deadline_timer,
                                         (uint64_t)CONFIG_BLE_HOST_SECURE_DEADLINE_S * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "security deadline timer start failed: %s", esp_err_to_name(err));
    }
}

static void sec_deadline_stop(void)
{
    (void)esp_timer_stop(s_sec_deadline_timer);
    (void)esp_timer_stop(s_sec_retry_timer);
}

static void sec_initiate(uint16_t conn, uint32_t gen)
{
#if CONFIG_BLE_HOST_INITIATE_SECURITY
    int rc = ble_gap_security_initiate(conn);
    if (rc == 0) {
        return;
    }
    if (rc == BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "security already in progress (central started it) gen=%" PRIu32, gen);
        return;
    }
    if (rc == BLE_HS_ENOTCONN) {
        return;
    }
    ESP_LOGW(TAG, "BLECLI_SEC_FAIL gen=%" PRIu32 " reason=initiate_rc_%d", gen, rc);
    if (s_sec_retries_left > 0) {
        s_sec_retries_left--;
        s_sec_retry_gen = gen;
        (void)esp_timer_stop(s_sec_retry_timer);
        (void)esp_timer_start_once(s_sec_retry_timer, SEC_RETRY_DELAY_US);
    }
#else
    (void)conn;
    (void)gen;
#endif
}

static void sec_deadline_run(void)
{
    uint32_t dgen = atomic_load(&s_deadline_gen);
    if (!host_deadline_should_terminate(s_link.up, host_reg_gen(&s_reg), dgen, s_link.secure)) {
        return; /* stale expiry */
    }
    ESP_LOGW(TAG, "BLECLI_SEC_TIMEOUT gen=%" PRIu32 " after_s=%d", dgen, CONFIG_BLE_HOST_SECURE_DEADLINE_S);
    int rc = ble_gap_terminate(s_link.conn, BLE_ERR_AUTH_FAIL);
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
        /* The unsecured link still holds the only slot and the deadline timer is one-shot:
         * log the rc and retry shortly, so the bound does not become unbounded. */
        ESP_LOGW(TAG, "security-deadline terminate failed: rc=%d, retrying in 2 s", rc);
        (void)esp_timer_stop(s_sec_deadline_timer);
        (void)esp_timer_start_once(s_sec_deadline_timer, SEC_RETRY_DELAY_US);
    }
}

static void sec_retry_run(void)
{
    if (s_link.up && !s_link.secure && s_link.gen == s_sec_retry_gen) {
        sec_initiate(s_link.conn, s_sec_retry_gen);
    }
}

/* ---- advertising (host task only, tech_spec 2.3.6) ---- */

static void adv_set_active(bool on)
{
    s_advertising = on;
    atomic_store(&s_adv_active_pub, on ? 1u : 0u);
}

static int adv_primary_id(void)
{
    int pid = host_reg_primary(&s_reg);
    if (pid < 0 && host_reg_count(&s_reg) > 0) {
        pid = 0;
    }
    return pid;
}

static const char *adv_mode_name(void)
{
    return (host_reg_primary(&s_reg) == s_reg.default_id) ? "disarmed" : "armed";
}

static esp_err_t adv_apply_payload(void)
{
    int pid = adv_primary_id();
    if (pid < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_host_client_cfg_t *p = host_reg_get(&s_reg, (uint8_t)pid);
    if (p == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t alen = 0;
    size_t rlen = 0;
    esp_err_t err = adv_build_adv(s_adv_buf, sizeof(s_adv_buf), &alen, p->adv_uuid128);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t *uu[HOST_REG_ABS_MAX];
    size_t n = 0;
    for (uint8_t i = 0; i < host_reg_count(&s_reg) && n < HOST_REG_ABS_MAX; i++) {
        const ble_host_client_cfg_t *c = host_reg_get(&s_reg, i);
        if (c == NULL || i == (uint8_t)pid) {
            continue;
        }
        if (c->adv_policy == BLE_HOST_ADV_ALWAYS) {
            uu[n++] = c->adv_uuid128;
        }
    }
    err = adv_build_rsp(s_rsp_buf, sizeof(s_rsp_buf), &rlen, CONFIG_BLE_HOST_DEVICE_NAME, uu, n);
    if (err != ESP_OK) {
        return err;
    }
    int rc = ble_gap_adv_set_data(s_adv_buf, (int)alen);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gap_adv_rsp_set_data(s_rsp_buf, (int)rlen);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data failed: rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void adv_schedule_retry(void)
{
    uint32_t delay_s = CONFIG_BLE_HOST_ADV_RETRY_MAX_S;
    if (s_adv_retry_idx < 5) {
        uint32_t d = 1u << s_adv_retry_idx; /* 1, 2, 4, 8, 16 s */
        if (d < delay_s) {
            delay_s = d;
        }
        s_adv_retry_idx++;
    }
    (void)esp_timer_stop(s_adv_retry_timer);
    esp_err_t err = esp_timer_start_once(s_adv_retry_timer, (uint64_t)delay_s * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "advertising retry timer start failed: %s", esp_err_to_name(err));
    }
}

static void adv_start_failed(const char *what, int rc)
{
    if (!s_adv_fail_logged) {
        s_adv_fail_logged = true; /* logged once per state change, not per attempt */
        ESP_LOGE(TAG, "advertising %s failed: rc=%d (retrying with backoff)", what, rc);
    }
    adv_schedule_retry();
}

static void adv_start_now(void)
{
    if (atomic_load(&s_state) != HS_UP) {
        return; /* advertising starts in sync_cb, which sets UP first */
    }
    if (s_link.up) {
        return; /* payload is applied at the next start */
    }
    if (ble_gap_adv_active()) {
        return;
    }
    esp_err_t err = adv_apply_payload();
    if (err != ESP_OK) {
        adv_start_failed("payload", (int)err);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0) {
        adv_start_failed("start", rc);
        return;
    }
    adv_set_active(true);
    s_adv_fail_logged = false;
    s_adv_retry_idx = 0;
    ESP_LOGI(TAG, "BLECLI_ADV_START mode=%s", adv_mode_name());
}

static void adv_stop_now(const char *reason)
{
    if (s_advertising || ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
        adv_set_active(false);
        ESP_LOGI(TAG, "BLECLI_ADV_STOP reason=%s", reason);
    }
}

static void adv_retry_run(void)
{
    if (!s_link.up && !s_advertising) {
        adv_start_now();
    }
}

static void host_pending_run(void)
{
    uint32_t bits = host_pending_take(&s_host_pending);
    if ((bits & HOST_PEND_ADV_RETRY) != 0) {
        adv_retry_run();
    }
    if ((bits & HOST_PEND_SEC_DEADLINE) != 0) {
        sec_deadline_run();
    }
    if ((bits & HOST_PEND_SEC_RETRY) != 0) {
        sec_retry_run();
    }
}

/* ---- GAP events (host task) ---- */

static void format_peer(const struct ble_gap_conn_desc *desc, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             desc->peer_id_addr.val[5], desc->peer_id_addr.val[4], desc->peer_id_addr.val[3],
             desc->peer_id_addr.val[2], desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
}

static void on_gap_connect(const struct ble_gap_event *event)
{
    if (event->connect.status != 0) {
        adv_start_now();
        return;
    }
    const uint16_t ch = event->connect.conn_handle;
    if (s_link.up) {
        /* REQ-FUNC-80(a): only one concurrent client. */
        (void)ble_gap_terminate(ch, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    char peer[18];
    peer[0] = '\0';
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(ch, &desc) == 0) {
        format_peer(&desc, peer);
    }
    uint32_t gen = gen_bump();
    link_set(ch, gen, false, peer);
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    adv_set_active(false); /* undirected advertising stops on connect */
    (void)esp_timer_stop(s_adv_retry_timer);
    s_sec_retries_left = CONFIG_BLE_HOST_SEC_INITIATE_RETRIES;
    sec_deadline_arm(gen);
    ESP_LOGI(TAG, "BLECLI_CONNECT gen=%" PRIu32 " peer=%s", gen, peer);
    sec_initiate(ch, gen);
    dispatch(BLE_HOST_EVT_CONNECT, ch, false, 0);
}

static void on_gap_disconnect(const struct ble_gap_event *event)
{
    const uint16_t ch = event->disconnect.conn.conn_handle;
    const int reason = event->disconnect.reason;
    if (!s_link.up || s_link.conn != ch) {
        return; /* the DISCONNECT of a rejected extra connection must not touch the real link */
    }
    link_clear();
    sec_deadline_stop();
    uint32_t gen = gen_bump();
    (void)gen;
    adv_set_active(false);
    adv_start_now(); /* REQ-REL-03 */
    dispatch(BLE_HOST_EVT_DISCONNECT, ch, false, reason);
}

static void on_gap_enc_change(const struct ble_gap_event *event)
{
    const uint16_t ch = event->enc_change.conn_handle;
    const int status = event->enc_change.status;
    if (!s_link.up || s_link.conn != ch) {
        return;
    }
    struct ble_gap_conn_desc desc;
    bool found = (ble_gap_conn_find(ch, &desc) == 0);
    bool secure = found && desc.sec_state.encrypted && desc.sec_state.bonded;
    const bool was_secure = s_link.secure;

    if (secure) {
        sec_deadline_stop();
        ESP_LOGI(TAG, "BLECLI_SEC_OK gen=%" PRIu32, s_link.gen);
    } else if (status != 0) {
        ESP_LOGW(TAG, "BLECLI_SEC_FAIL gen=%" PRIu32 " reason=%d", s_link.gen, status);
    }
    if (was_secure && !secure) {
        /* REQ-FUNC-64: a loss of security is a new generation. */
        uint32_t gen = gen_bump();
        portENTER_CRITICAL(&s_link_mux);
        s_link.gen = gen;
        portEXIT_CRITICAL(&s_link_mux);
        sec_deadline_arm(gen);
    }
    portENTER_CRITICAL(&s_link_mux);
    s_link.secure = secure;
    portEXIT_CRITICAL(&s_link_mux);
    dispatch(BLE_HOST_EVT_ENC_CHANGE, ch, secure, status);
}

static void on_gap_subscribe(const struct ble_gap_event *event)
{
    if (!s_link.up || s_link.conn != event->subscribe.conn_handle) {
        return;
    }
    ble_host_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = BLE_HOST_EVT_SUBSCRIBE;
    evt.conn_handle = event->subscribe.conn_handle;
    evt.gen = host_reg_gen(&s_reg);
    evt.secure = s_link.secure;
    evt.attr_handle = event->subscribe.attr_handle;
    evt.notify = (event->subscribe.cur_notify != 0);
    evt.prev_notify = (event->subscribe.prev_notify != 0);
    switch (event->subscribe.reason) {
    case BLE_GAP_SUBSCRIBE_REASON_WRITE:
        evt.sub_reason = BLE_HOST_SUB_WRITE;
        break;
    case BLE_GAP_SUBSCRIBE_REASON_RESTORE:
        evt.sub_reason = BLE_HOST_SUB_RESTORE;
        break;
    default:
        evt.sub_reason = BLE_HOST_SUB_TERM;
        break;
    }
    portENTER_CRITICAL(&s_link_mux);
    memcpy(evt.peer_addr, s_link.peer, sizeof(evt.peer_addr));
    portEXIT_CRITICAL(&s_link_mux);
    evt.peer_addr[sizeof(evt.peer_addr) - 1] = '\0';
    dispatch_evt(&evt);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        on_gap_connect(event);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        on_gap_disconnect(event);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        on_gap_enc_change(event);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        on_gap_subscribe(event);
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (s_link.up && s_link.conn == event->mtu.conn_handle) {
            portENTER_CRITICAL(&s_link_mux);
            s_link.mtu = event->mtu.value;
            portEXIT_CRITICAL(&s_link_mux);
            ble_host_evt_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = BLE_HOST_EVT_MTU;
            evt.conn_handle = event->mtu.conn_handle;
            evt.gen = host_reg_gen(&s_reg);
            evt.secure = s_link.secure;
            evt.mtu = event->mtu.value;
            dispatch_evt(&evt);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            (void)ble_store_util_delete_peer(&desc.peer_id_addr); /* REQ-FUNC-80(b) */
        }
        dispatch(BLE_HOST_EVT_REPEAT_PAIRING, event->repeat_pairing.conn_handle, false, 0);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_set_active(false);
        if (!s_link.up) {
            adv_start_now();
        }
        return 0;

    default:
        return 0;
    }
}

/* ---- GATT database change detection (tech_spec 2.3.8) ---- */

static uint32_t crc_bytes(uint32_t crc, const void *p, size_t n)
{
    return esp_rom_crc32_le(crc, (const uint8_t *)p, (uint32_t)n);
}

static uint32_t crc_uuid(uint32_t crc, const ble_uuid_t *u)
{
    if (u == NULL) {
        return crc;
    }
    switch (u->type) {
    case BLE_UUID_TYPE_16: {
        uint16_t v = BLE_UUID16(u)->value;
        return crc_bytes(crc, &v, sizeof(v));
    }
    case BLE_UUID_TYPE_32: {
        uint32_t v = BLE_UUID32(u)->value;
        return crc_bytes(crc, &v, sizeof(v));
    }
    case BLE_UUID_TYPE_128:
        return crc_bytes(crc, BLE_UUID128(u)->value, 16);
    default:
        return crc;
    }
}

static uint32_t gatt_db_crc(void)
{
    uint32_t crc = 0;
    for (uint8_t i = 0; i < host_reg_count(&s_reg); i++) {
        const struct ble_gatt_svc_def *s = host_reg_get(&s_reg, i)->svcs;
        for (; s != NULL && s->type != BLE_GATT_SVC_TYPE_END; s++) {
            uint8_t t = s->type;
            crc = crc_bytes(crc, &t, 1);
            crc = crc_uuid(crc, s->uuid);
            const struct ble_gatt_chr_def *c = s->characteristics;
            for (; c != NULL && c->uuid != NULL; c++) {
                crc = crc_uuid(crc, c->uuid);
                uint16_t f = (uint16_t)c->flags;
                crc = crc_bytes(crc, &f, sizeof(f));
            }
        }
    }
    return crc;
}

static void gatt_changed_check(void)
{
    uint32_t now_crc = gatt_db_crc();
    nvs_handle_t h;
    esp_err_t err = nvs_open(GATT_CRC_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GATT CRC check skipped: nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    uint32_t stored = 0;
    err = nvs_get_u32(h, GATT_CRC_NVS_KEY, &stored);
    if (err != ESP_OK || stored != now_crc) {
        ESP_LOGI(TAG, "GATT database changed (crc 0x%08" PRIx32 " -> 0x%08" PRIx32 "); sending Service Changed",
                 stored, now_crc);
        ble_svc_gatt_changed(0x0001, 0xFFFF);
        err = nvs_set_u32(h, GATT_CRC_NVS_KEY, now_crc);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GATT CRC store failed (%s)", esp_err_to_name(err));
        }
    }
    nvs_close(h);
}

/* ---- host lifecycle callbacks ---- */

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    uint32_t exp = HS_UP;
    (void)atomic_compare_exchange_strong(&s_state, &exp, (uint32_t)HS_STARTING);
    link_clear();
    adv_set_active(false);
    sec_deadline_stop();
    (void)esp_timer_stop(s_adv_retry_timer);
    (void)host_pending_take(&s_host_pending); /* the reset invalidates every pending action */
    (void)gen_bump();
    dispatch(BLE_HOST_EVT_HOST_RESET, BLE_HS_CONN_HANDLE_NONE, false, reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=ensure_addr rc=%d", rc);
        atomic_store(&s_state, HS_FAILED);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=infer_addr rc=%d", rc);
        atomic_store(&s_state, HS_FAILED);
        return;
    }
    atomic_store(&s_state, HS_UP);
    if (!s_synced_once) {
        s_synced_once = true;
        (void)esp_timer_stop(s_sync_wd_timer);
        gatt_changed_check();
        ESP_LOGI(TAG, "BLECLI_HOST_UP");
        dispatch(BLE_HOST_EVT_SYNC, BLE_HS_CONN_HANDLE_NONE, false, 0);
    } else {
        ESP_LOGW(TAG, "BLECLI_HOST_RESYNC");
    }
    host_pending_run(); /* inline, never posted from the host task (R3-05) */
    adv_start_now();
}

static void host_task_fn(void *param)
{
    (void)param;
    s_host_task = xTaskGetCurrentTaskHandle();
    nimble_port_run(); /* blocks; no runtime path calls nimble_port_stop() */
    vTaskDelete(NULL);
}

/* ---- public API: lifecycle ---- */

static esp_err_t create_timer(esp_timer_handle_t *out, esp_timer_cb_t cb, const char *name)
{
    const esp_timer_create_args_t args = {
        .callback = cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
    };
    return esp_timer_create(&args, out);
}

esp_err_t ble_host_init(void)
{
    uint32_t exp = HS_UNINIT;
    if (!atomic_compare_exchange_strong(&s_state, &exp, (uint32_t)HS_INIT)) {
        return ESP_ERR_INVALID_STATE;
    }
    host_reg_init(&s_reg, CONFIG_BLE_HOST_MAX_CLIENTS);
    host_exec_init(&s_exec);
    link_clear();
    atomic_store(&s_gen_pub, host_reg_gen(&s_reg));

    s_exec_mtx = xSemaphoreCreateMutexStatic(&s_exec_mtx_buf);
    s_exec_sem = xSemaphoreCreateBinaryStatic(&s_exec_sem_buf);
    esp_err_t err = ESP_OK;
    if (s_exec_mtx == NULL || s_exec_sem == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = create_timer(&s_sync_wd_timer, sync_wd_timer_cb, "bh_sync_wd");
    }
    if (err == ESP_OK) {
        err = create_timer(&s_adv_retry_timer, adv_retry_timer_cb, "bh_adv_retry");
    }
    if (err == ESP_OK) {
        err = create_timer(&s_sec_deadline_timer, sec_deadline_timer_cb, "bh_sec_dl");
    }
    if (err == ESP_OK) {
        err = create_timer(&s_sec_retry_timer, sec_retry_timer_cb, "bh_sec_rt");
    }
    if (err != ESP_OK) {
        atomic_store(&s_state, HS_FAILED);
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=%s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t ble_host_register_client(const ble_host_client_cfg_t *cfg, ble_host_client_id_t *out_id)
{
    return host_reg_register(&s_reg, atomic_load(&s_state) == HS_INIT, cfg, out_id);
}

esp_err_t ble_host_start(void)
{
    uint32_t exp = HS_INIT;
    if (!atomic_compare_exchange_strong(&s_state, &exp, (uint32_t)HS_STARTING)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (host_reg_count(&s_reg) == 0) {
        atomic_store(&s_state, HS_FAILED);
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=no_clients");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NS5_TEST_FAIL_HOST_INIT
    /* BUILD-TEST ONLY (hook H-8): force a bring-up failure before the radio is
     * touched, so the only error line of the boot is BLECLI_HOST_FAIL. */
    atomic_store(&s_state, HS_FAILED);
    ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=test_injected");
    return ESP_FAIL;
#endif

    if (!s_mem_released_once) {
        /* One-shot, BLE-only build: release classic-BT memory (REQ-FUNC-73). */
        esp_err_t mr = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (mr != ESP_OK) {
            ESP_LOGW(TAG, "classic-BT memory release returned %s (not fatal)", esp_err_to_name(mr));
        }
        s_mem_released_once = true;
    }

    /* nimble_port_init() performs the full controller bring-up itself on this
     * target (see NETSHELL32-2 finding); do not duplicate it here. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        atomic_store(&s_state, HS_FAILED);
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=nimble_port_init:%s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr; /* REQ-FUNC-80(d) */

    /* Just Works, single localised parameter block (moved from NETSHELL32-2). */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set(CONFIG_BLE_HOST_DEVICE_NAME);

    /* Registration order == dispatch order == add_svcs order (invariant I-1). */
    int rc = 0;
    for (uint8_t i = 0; i < host_reg_count(&s_reg) && rc == 0; i++) {
        rc = ble_gatts_count_cfg(host_reg_get(&s_reg, i)->svcs);
    }
    for (uint8_t i = 0; i < host_reg_count(&s_reg) && rc == 0; i++) {
        rc = ble_gatts_add_svcs(host_reg_get(&s_reg, i)->svcs);
    }
    if (rc != 0) {
        /* The only path that may call nimble_port_deinit(): boot unwind. */
        nimble_port_deinit();
        atomic_store(&s_state, HS_FAILED);
        ESP_LOGE(TAG, "BLECLI_HOST_FAIL err=gatt_register rc=%d", rc);
        return ESP_FAIL;
    }

    /* Exactly two NPL events, once, after nimble_port_init() created the pool. */
    ble_npl_event_init(&s_exec_ev, exec_ev_fn, NULL);
    ble_npl_event_init(&s_kick_ev, kick_ev_fn, NULL);

    nimble_port_freertos_init(host_task_fn);

    err = esp_timer_start_once(s_sync_wd_timer, SYNC_WATCHDOG_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync watchdog start failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool ble_host_is_up(void)
{
    return atomic_load(&s_state) == HS_UP;
}

bool ble_host_is_starting(void)
{
    return atomic_load(&s_state) == HS_STARTING;
}

/* ---- public API: client helpers ---- */

esp_err_t ble_host_exec(ble_host_exec_fn_t fn, uintptr_t arg, uint32_t timeout_ms, int32_t *out_result)
{
    if (s_exec_mtx == NULL || s_exec_sem == NULL) {
        return ESP_ERR_BLE_HOST_NOT_UP;
    }
    return host_exec_call(&s_exec, &s_exec_ops, fn, arg, timeout_ms, out_result);
}

esp_err_t ble_host_set_adv_primary(ble_host_client_id_t id, bool primary)
{
    esp_err_t err = host_reg_set_primary(&s_reg, id, primary);
    if (err != ESP_OK) {
        return err;
    }
    if (atomic_load(&s_state) != HS_UP || s_link.up) {
        return ESP_OK; /* stored; used at the next start */
    }
    if (s_advertising) {
        adv_stop_now("payload_switch");
        adv_start_now();
    }
    return ESP_OK;
}

void ble_host_note_engaged(ble_host_client_id_t id, uint32_t gen)
{
    host_reg_note_engaged(&s_reg, id, gen);
    atomic_store(&s_engaged_pub, s_reg.engaged_mask);
}

esp_err_t ble_host_release_link(ble_host_client_id_t id, uint32_t gen)
{
    if (!host_reg_release_allowed(&s_reg, id, gen, s_link.up)) {
        return ESP_ERR_INVALID_STATE; /* link kept */
    }
    int rc = ble_gap_terminate(s_link.conn, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

uint32_t ble_host_current_gen(void)
{
    return atomic_load(&s_gen_pub);
}

bool ble_host_link_secure(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return false;
    }
    return desc.sec_state.encrypted && desc.sec_state.bonded;
}

uint16_t ble_host_att_mtu(uint16_t conn_handle)
{
    uint16_t m = ble_att_mtu(conn_handle);
    return (m == 0) ? 23 : m;
}

esp_err_t ble_host_notify(uint16_t conn_handle, uint16_t val_handle, const uint8_t *data, size_t len,
                          uint32_t expected_gen)
{
    if (data == NULL || len == 0 || len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Single choke point (ADV-IMPL-04): ble_gatts_notify_custom() checks neither the CCCD
     * nor security, so no notification leaves the device unless the link is up, is the
     * given handle, belongs to the expected generation and is encrypted and bonded NOW. */
    bool up = false;
    uint16_t cur_conn = 0;
    uint32_t cur_gen = 0;
    bool cur_secure = false;
    portENTER_CRITICAL(&s_link_mux);
    up = s_link.up;
    cur_conn = s_link.conn;
    cur_gen = s_link.gen;
    cur_secure = s_link.secure;
    portEXIT_CRITICAL(&s_link_mux);
    if (!up || cur_conn != conn_handle || expected_gen == 0u || cur_gen != expected_gen ||
        expected_gen != atomic_load(&s_gen_pub) || !cur_secure || !ble_host_link_secure(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(conn_handle, val_handle, om); /* consumes om */
    if (rc == 0) {
        return ESP_OK;
    }
    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
        return ESP_ERR_NO_MEM;
    }
    if (rc == BLE_HS_ENOTCONN) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t ble_host_terminate(uint16_t conn_handle)
{
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == BLE_HS_ENOTCONN) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t ble_host_get_link(uint16_t *conn_handle, uint32_t *gen, bool *secure, char peer_addr[18])
{
    bool up;
    uint16_t c;
    uint32_t g;
    bool sec;
    char peer[18];
    portENTER_CRITICAL(&s_link_mux);
    up = s_link.up;
    c = s_link.conn;
    g = s_link.gen;
    sec = s_link.secure;
    memcpy(peer, s_link.peer, sizeof(peer));
    portEXIT_CRITICAL(&s_link_mux);
    if (!up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (conn_handle != NULL) {
        *conn_handle = c;
    }
    if (gen != NULL) {
        *gen = g;
    }
    if (secure != NULL) {
        *secure = sec;
    }
    if (peer_addr != NULL) {
        memcpy(peer_addr, peer, sizeof(peer));
        peer_addr[17] = '\0';
    }
    return ESP_OK;
}

bool ble_host_is_advertising(void)
{
    return atomic_load(&s_adv_active_pub) != 0;
}

uint32_t ble_host_engaged_mask(void)
{
    return atomic_load(&s_engaged_pub);
}

uint32_t ble_host_cb_max_us(void)
{
    return atomic_load(&s_cb_max_us);
}

uint32_t ble_host_ota_rx_max_us(void)
{
    return atomic_load(&s_ota_rx_max_us);
}

#if CONFIG_NS5_TEST_STALL_HOST
/* BUILD-TEST ONLY (hook H-10): block the NimBLE host task for `ms` with vTaskDelay
 * (never a busy loop, so the task watchdog is not tripped). Done from a short-lived
 * helper task so the console command returns at once and `reboot` can be typed
 * while the host task is stalled. */
static int32_t stall_fn(uintptr_t arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)arg));
    return 0;
}

static void stall_task_fn(void *arg)
{
    int32_t r = 0;
    (void)ble_host_exec(stall_fn, (uintptr_t)arg, 1000, &r);
    vTaskDelete(NULL);
}

esp_err_t ble_host_test_stall(uint32_t ms)
{
    if (ms == 0 || ms > 30000) {
        return ESP_ERR_INVALID_ARG;
    }
    BaseType_t ok = xTaskCreate(stall_task_fn, "ns5_stall", 2048, (void *)(uintptr_t)ms, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
#endif

void ble_host_note_ota_rx_us(uint32_t us)
{
    /* Single writer (the host task), so a load/store pair is sufficient. */
    if (us > atomic_load(&s_ota_rx_max_us)) {
        atomic_store(&s_ota_rx_max_us, us);
    }
}
