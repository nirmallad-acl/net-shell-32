/**
 * @file ble_cli.c
 * @brief BLE UART-style CLI client of ble_host: GATT service, RX path, worker, TX drain
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
 * REQ-FUNC-01..11, 13, 14, 16, 48, 49, 60..64, REQ-SEC-01..04,
 * tech_spec.md section 2.5.
 *
 * Threading: the RX access callback and the host-event callback run on the
 * NimBLE host task and take no mutex. All command execution and all response
 * transmission happen on the single worker task (blecli_wrk), so whole lines
 * never interleave. Only diag response lines ever enter the TX queue and nothing
 * else can reach the TX characteristic (REQ-FUNC-11). Response CONTENT is never
 * logged (REQ-SEC-04): only lengths.
 */
#include "ble_cli.h"

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
#include "sdkconfig.h"

#include "ble_host_client.h"
#include "diag_cmds.h"

#include "cli_frag.h"
#include "cli_line_asm.h"
#include "cli_tx_gate.h"

static const char *TAG = "ble_cli";

#define BLECLI_RESMON_MIN_GAP_US 10000000LL /* disconnect resource dumps at most every 10 s */

#define BLECLI_RX_WRITE_MAX   512u
#define BLECLI_TX_RETRIES     5u   /* retries after the first attempt, 20 ms apart (bounded) */
#define BLECLI_TX_RETRY_MS    20u
#define BLECLI_SINK_ENQ_MS    200u
#define BLECLI_FLUSH_SETTLE_MS 100u
#define BLECLI_LOG_GAP_US     1000000LL /* rate limit of repeated reject lines */

/* ---- GATT service: project-specific random 128-bit UUIDs (little-endian bytes) ----
 * service 8F3B1A20-5C6D-4E7F-9A0B-1C2D3E4F5A60, RX ...21..., TX ...22... */
#define CLI_UUID_LE(b12) 0x60, 0x5A, 0x4F, 0x3E, 0x2D, 0x1C, 0x0B, 0x9A, 0x7F, 0x4E, 0x6D, 0x5C, (b12), 0x1A, 0x3B, 0x8F

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(CLI_UUID_LE(0x20));
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(CLI_UUID_LE(0x21));
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(CLI_UUID_LE(0x22));
static const uint8_t       s_adv_uuid[16] = { CLI_UUID_LE(0x20) };

typedef struct {
    uint32_t gen;
    uint32_t t_rx_ms;
    uint8_t  kind;    /* cli_kind_t */
    int8_t   cmd_id;  /* diag_cmd_id_t or -1 */
    uint16_t pad;
} cmd_item_t;

typedef struct {
    uint32_t gen;
    uint8_t  len;     /* whole response line including the LF */
    char     data[DIAG_LINE_MAX];
} tx_item_t;

typedef struct {
    uint32_t gen;
    uint32_t t_rx_ms;
} rx_ctx_t;

/* ---- static storage (zero heap after init, REQ-MEM-01) ---- */
static StaticQueue_t s_cmd_q_buf;
static uint8_t       s_cmd_q_store[CONFIG_BLE_CLI_CMD_QUEUE_DEPTH * sizeof(cmd_item_t)];
static StaticQueue_t s_tx_q_buf;
static uint8_t       s_tx_q_store[CONFIG_BLE_CLI_TX_QUEUE_DEPTH * sizeof(tx_item_t)];
static QueueHandle_t s_cmd_q;
static QueueHandle_t s_tx_q;
static TaskHandle_t  s_worker;

static uint8_t        s_rx_scratch[BLECLI_RX_WRITE_MAX]; /* host task only */
static cli_line_asm_t s_asm;                             /* host task only */
static char           s_worker_line[DIAG_LINE_MAX + 1];  /* worker task only */
static char           s_reason_buf[12];                  /* host task only */

static ble_host_client_id_t s_client_id;
static uint16_t             s_tx_val_handle;
static bool                 s_inited;

/* Host-task-only link bookkeeping. */
static uint32_t s_link_gen;     /* generation of the current link, 0 = none */
static bool     s_was_secure;
static int64_t  s_last_reject_log_us;
static int64_t  s_last_resmon_us;   /* host task only: rate limit of the disconnect resource dump */

/* Shared between the host task and the worker. */
static _Atomic uint32_t s_tx_subscribed;
static cli_resync_t     s_resync;           /* worker task only (tx_send_line runs on the worker) */
static cli_latch_t      s_latch;           /* input-overflow latch: drop every later item until drained */
static _Atomic uint32_t s_tx_failed_gen;    /* generation whose link is gone (terminal TX failure) */
static _Atomic uint32_t s_tx_dirty_gen;     /* generation that lost or dropped a line */
static _Atomic uint32_t s_resmon_pending;

/* The generation of the current link if it is encrypted and bonded, else 0. */
static uint32_t cur_secure_gen(uint16_t *conn_out)
{
    uint16_t c = 0;
    uint32_t g = 0;
    bool sec = false;
    if (ble_host_get_link(&c, &g, &sec, NULL) != ESP_OK || !sec) {
        return 0;
    }
    if (conn_out != NULL) {
        *conn_out = c;
    }
    return g;
}

static int cmd_lookup(const char *name, size_t len)
{
    diag_cmd_id_t id;
    if (diag_cmd_lookup_ble(name, len, &id) != ESP_OK) {
        return -1;
    }
    return (int)id;
}

/* ---- TX path (worker task, and the one inline path of ble_sink_write) ---- */

static void tx_mark_dirty(uint32_t gen)
{
    atomic_store(&s_tx_dirty_gen, gen);
}

static void tx_purge_queue(void)
{
    tx_item_t it;
    while (xQueueReceive(s_tx_q, &it, 0) == pdTRUE) {
        /* dropped: they belong to the generation whose TX path just failed */
    }
}

/* Sends one whole line (data includes the LF) as ATT_MTU-3 sized notifications. */
static esp_err_t tx_send_line(uint32_t gen, const char *data, size_t len, int64_t deadline_us)
{
    uint16_t conn = 0;
    uint32_t cg = cur_secure_gen(&conn);
    switch (cli_tx_decide(gen, cg, atomic_load(&s_tx_failed_gen), atomic_load(&s_tx_subscribed) != 0u)) {
    case CLI_TX_DROP_STALE:
        tx_mark_dirty(gen); /* stale generation or link not secure: dropped */
#if CONFIG_BLE_CLI_DEBUG_TX_LOG
        ESP_LOGI(TAG, "BLECLI_TX_SUPPRESS gen=%" PRIu32 " len=%u reason=not_secure", gen, (unsigned)len);
#endif
        return ESP_ERR_INVALID_STATE;
    case CLI_TX_DROP_FAILED:
        tx_mark_dirty(gen);
        return ESP_ERR_INVALID_STATE;
    case CLI_TX_DROP_UNSUBSCRIBED:
        tx_mark_dirty(gen); /* inert subscription: nothing is ever sent unsubscribed (REQ-FUNC-61) */
#if CONFIG_BLE_CLI_DEBUG_TX_LOG
        ESP_LOGI(TAG, "BLECLI_TX_SUPPRESS gen=%" PRIu32 " len=%u reason=not_subscribed", gen, (unsigned)len);
#endif
        return ESP_OK;
    default:
        break;
    }

    /* A previous line of this generation was cut off mid-line by a transient failure: send one
     * LF first so the peer does not glue this line onto the fragment (best effort, ADV-R2-01). */
    if (cli_resync_take(&s_resync, gen)) {
        static const uint8_t lf = (uint8_t)'\n';
        (void)ble_host_notify(conn, s_tx_val_handle, &lf, 1, gen);
    }

    uint16_t mtu = ble_host_att_mtu(conn);
    size_t payload = (mtu > 3u) ? (size_t)(mtu - 3u) : CLI_FRAG_MIN_PAYLOAD;
    cli_frag_plan_t plan;
    if (cli_frag_plan(len, payload, &plan) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < plan.count; i++) {
        if (cur_secure_gen(NULL) != gen) { /* re-check before every slice */
            tx_mark_dirty(gen);
            return ESP_ERR_INVALID_STATE;
        }
        size_t off = 0;
        size_t slen = cli_frag_slice(&plan, i, &off);
        esp_err_t err = ESP_FAIL;
        for (unsigned attempt = 0; attempt <= BLECLI_TX_RETRIES; attempt++) {
            err = ble_host_notify(conn, s_tx_val_handle, (const uint8_t *)&data[off], slen, gen);
            if (err != ESP_ERR_NO_MEM) {
                break;
            }
            if (deadline_us != 0 && esp_timer_get_time() >= deadline_us) {
                tx_mark_dirty(gen);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(BLECLI_TX_RETRY_MS));
        }
        if (err != ESP_OK) {
            /* Only "the link is gone / no longer secure" is terminal for the generation
             * (ADV-IMPL-05). Congestion or another transient error drops this line and the
             * queued ones, but the next line may succeed, and a success clears the mark. */
            if (cli_tx_failure_is_terminal(err)) {
                atomic_store(&s_tx_failed_gen, gen);
            }
            cli_resync_note_failure(&s_resync, gen, i, err); /* i slices were already on air */
            tx_mark_dirty(gen);
            unsigned waiting = (unsigned)uxQueueMessagesWaiting(s_tx_q);
            tx_purge_queue();
            ESP_LOGW(TAG, "BLECLI_TX_FAIL gen=%" PRIu32 " dropped=%u", gen, waiting);
            return err;
        }
    }
    if (atomic_load(&s_tx_failed_gen) == gen) {
        atomic_store(&s_tx_failed_gen, 0u);
    }
#if CONFIG_BLE_CLI_DEBUG_TX_LOG
    ESP_LOGI(TAG, "BLECLI_TX gen=%" PRIu32 " len=%u", gen, (unsigned)len);
#endif
    return ESP_OK;
}

/* Worker only. With deadline_us != 0 (reboot path) the remaining queued lines of
 * the generation are dropped once the deadline passes. Returns true if anything
 * was dequeued. */
static bool drain_tx_queue(int64_t deadline_us)
{
    bool any = false;
    tx_item_t it;
    while (xQueueReceive(s_tx_q, &it, 0) == pdTRUE) {
        any = true;
        if (deadline_us != 0 && esp_timer_get_time() >= deadline_us) {
            tx_mark_dirty(it.gen);
            continue;
        }
        (void)tx_send_line(it.gen, it.data, it.len, deadline_us);
    }
    return any;
}

/* ---- sinks (REQ-FUNC-14, ADV-CONC-04) ---- */

static esp_err_t ble_sink_write(const diag_ctx_t *ctx, const char *line, size_t len)
{
    if (ctx == NULL || line == NULL || len == 0 || len > DIAG_LINE_MAX - 1u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cli_sink_admit(ctx->gen, cur_secure_gen(NULL), atomic_load(&s_tx_failed_gen))) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskGetCurrentTaskHandle() == s_worker) {
        /* The worker never waits on the queue only it drains: drain inline (keeps
         * order), then transmit this line directly. */
        (void)drain_tx_queue(ctx->deadline_us);
        memcpy(s_worker_line, line, len);
        s_worker_line[len] = '\n';
        return tx_send_line(ctx->gen, s_worker_line, len + 1u, ctx->deadline_us);
    }

    tx_item_t it;
    it.gen = ctx->gen;
    it.len = (uint8_t)(len + 1u);
    memcpy(it.data, line, len);
    it.data[len] = '\n';
    if (xQueueSend(s_tx_q, &it, pdMS_TO_TICKS(BLECLI_SINK_ENQ_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* the link is not draining */
    }
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker); /* on air as soon as the worker runs, not at the 1 s timeout */
    }
    return ESP_OK;
}

static esp_err_t ble_sink_flush(const diag_ctx_t *ctx, uint32_t timeout_ms)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t t_end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (ctx->deadline_us != 0 && t_end > ctx->deadline_us) {
        t_end = ctx->deadline_us;
    }
    if (xTaskGetCurrentTaskHandle() == s_worker) {
        (void)drain_tx_queue(t_end);
    } else {
        while (uxQueueMessagesWaiting(s_tx_q) > 0 && esp_timer_get_time() < t_end) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    esp_err_t r = (atomic_load(&s_tx_dirty_gen) == ctx->gen || atomic_load(&s_tx_failed_gen) == ctx->gen)
                      ? ESP_FAIL
                      : ESP_OK;
    int64_t rem_us = t_end - esp_timer_get_time();
    if (rem_us > 0) {
        uint32_t settle = (uint32_t)(rem_us / 1000);
        if (settle > BLECLI_FLUSH_SETTLE_MS) {
            settle = BLECLI_FLUSH_SETTLE_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(settle)); /* let the controller put it on air */
    }
    return r;
}

static bool ble_is_current(const diag_ctx_t *ctx)
{
    return ctx != NULL && cur_secure_gen(NULL) == ctx->gen && ctx->gen != 0u;
}

/* ---- worker ---- */

static void log_rx_reject(uint32_t gen, const char *reason)
{
    ESP_LOGW(TAG, "BLECLI_RX_REJECT gen=%" PRIu32 " reason=%s", gen, reason);
}

static void handle_cmd(const cmd_item_t *it)
{
    if (!cli_cmd_should_run(it->gen, cur_secure_gen(NULL))) {
        return; /* stale generation: dropped, never executed (REQ-FUNC-62/64) */
    }
    const diag_ctx_t ctx = {
        .transport = DIAG_TRANSPORT_BLE,
        .gen = it->gen,
        .deadline_us = 0,
        .write_line = ble_sink_write,
        .flush = ble_sink_flush,
        .is_current = ble_is_current,
        .t_rx_ms = it->t_rx_ms,
    };
    switch ((cli_kind_t)it->kind) {
    case CLI_KIND_CMD:
        (void)diag_cmd_run((diag_cmd_id_t)it->cmd_id, &ctx);
        break;
    case CLI_KIND_ERR_OVERSIZED:
        log_rx_reject(it->gen, "oversized_input");
        (void)diag_emit_error(&ctx, "oversized_input", NULL, NULL);
        break;
    case CLI_KIND_ERR_INVALID_CHAR:
        log_rx_reject(it->gen, "invalid_argument");
        (void)diag_emit_error(&ctx, "invalid_argument", NULL, NULL);
        break;
    case CLI_KIND_ERR_UNKNOWN:
        (void)diag_emit_error(&ctx, "unknown_command", NULL, NULL);
        break;
    case CLI_KIND_ERR_ARGS:
        log_rx_reject(it->gen, "invalid_argument");
        (void)diag_emit_error(&ctx, "invalid_argument", diag_cmd_name((diag_cmd_id_t)it->cmd_id), NULL);
        break;
    default:
        break;
    }
}

/* Called once the command queue is empty with the latch set: exactly one ERR busy
 * for the episode, then the latch is cleared (tech_spec.md 2.5.2 step 5). */
static void handle_overflow_clear(void)
{
    uint32_t gen = 0;
    if (!cli_latch_ready_to_clear(&s_latch, true, &gen)) {
        return;
    }
    if (gen != 0u && gen == cur_secure_gen(NULL)) {
        const diag_ctx_t ctx = {
            .transport = DIAG_TRANSPORT_BLE,
            .gen = gen,
            .write_line = ble_sink_write,
            .flush = ble_sink_flush,
            .is_current = ble_is_current,
        };
        (void)diag_emit_error(&ctx, "busy", NULL, "reason=input_overflow");
    }
    cli_latch_clear(&s_latch);
}

static void worker_fn(void *arg)
{
    (void)arg;
    for (;;) {
        /* Bounded wait: woken by the RX callback, by session lines and by host
         * events; the 1 s timeout is a safety net only. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        for (;;) {
            bool did = drain_tx_queue(0);
            cmd_item_t it;
            if (xQueueReceive(s_cmd_q, &it, 0) == pdTRUE) {
                handle_cmd(&it);
                did = true;
            } else if (cli_latch_active(&s_latch)) {
                handle_overflow_clear();
                did = true;
            }
            if (!did) {
                break;
            }
        }
        if (atomic_exchange(&s_resmon_pending, 0u) != 0u) {
            diag_resmon_emit("ble_disconnect"); /* off the host task, REQ-MEM */
        }
    }
}

/* ---- RX path (host task, D-02) ---- */

static bool push_cmd(void *ctx)
{
    return xQueueSend(s_cmd_q, ctx, 0) == pdTRUE;
}

static void on_line_result(const cli_result_t *r, void *user)
{
    const rx_ctx_t *c = (const rx_ctx_t *)user;
    cmd_item_t it = {
        .gen = c->gen,
        .t_rx_ms = c->t_rx_ms,
        .kind = (uint8_t)r->kind,
        .cmd_id = (int8_t)r->cmd_id,
        .pad = 0,
    };
    /* While the latch is set every later item is dropped (order is preserved). */
    if (cli_rx_offer(&s_latch, c->gen, push_cmd, &it) == CLI_RX_DROPPED_FULL) {
        log_rx_reject(c->gen, "queue_full");
    }
}

static int gatt_access_rx(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const uint32_t gen = ble_host_current_gen();
    const int64_t now_us = esp_timer_get_time();

    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    if (total_len == 0u || total_len > BLECLI_RX_WRITE_MAX) {
        if (now_us - s_last_reject_log_us >= BLECLI_LOG_GAP_US) {
            s_last_reject_log_us = now_us;
            log_rx_reject(gen, "oversized_input");
        }
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    /* REQ-FUNC-60: live encrypted AND bonded check before a single byte is copied. */
    struct ble_gap_conn_desc desc;
    const bool found = (ble_gap_conn_find(conn_handle, &desc) == 0);
    const cli_sec_verdict_t sv =
        cli_rx_sec_verdict(found && desc.sec_state.encrypted, found && desc.sec_state.bonded);
    if (sv != CLI_SEC_OK) {
        /* Nothing is buffered. 0x05 prompts Android to bond; 0x0F if the link is not even
         * encrypted (defensive: the stack normally rejects such a write first). */
        if (now_us - s_last_reject_log_us >= BLECLI_LOG_GAP_US) {
            s_last_reject_log_us = now_us;
            log_rx_reject(gen, "not_secure");
        }
        return (sv == CLI_SEC_NEED_ENCRYPTION) ? BLE_ATT_ERR_INSUFFICIENT_ENC : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    (void)cli_line_asm_sync(&s_asm, gen); /* REQ-FUNC-62: nothing survives a generation change */
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_scratch, sizeof(s_rx_scratch), &out_len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const rx_ctx_t rctx = { .gen = gen, .t_rx_ms = (uint32_t)(now_us / 1000) };
    (void)cli_line_asm_feed(&s_asm, s_rx_scratch, out_len, cmd_lookup, on_line_result, (void *)&rctx);
    ble_host_note_engaged(s_client_id, gen);
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
    return 0;
}

static int gatt_access_tx(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* Notify-only: no direct read or write is ever legal here. */
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_gatt_chrs[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = gatt_access_rx,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
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

/* ---- host events (host task) ---- */

static void link_lost(const char *reason, uint32_t old_gen, bool was_secure)
{
    cli_line_asm_reset(&s_asm, 0);
    cli_latch_clear(&s_latch);
    unsigned din = (unsigned)uxQueueMessagesWaiting(s_cmd_q);
    unsigned dout = (unsigned)uxQueueMessagesWaiting(s_tx_q);
    diag_session_cancel(DIAG_TRANSPORT_BLE, old_gen, DIAG_CANCEL_DISCONNECT); /* lock-free */
    ESP_LOGI(TAG, "BLECLI_DISCONNECT gen=%" PRIu32 " reason=%s dropped_in=%u dropped_out=%u", old_gen, reason,
             din, dout);
    /* The resource dump (about 0.7 KB of UART) only for a link that had been secure, and at
     * most every 10 s: an unauthenticated connect/disconnect loop must not drive the
     * console (REV-SEC-01, ADV-IMPL-12). */
    const int64_t now = esp_timer_get_time();
    if (was_secure && (s_last_resmon_us == 0 || now - s_last_resmon_us >= BLECLI_RESMON_MIN_GAP_US)) {
        s_last_resmon_us = now;
        atomic_store(&s_resmon_pending, 1u);
    }
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
}

static void log_subscribe(uint32_t gen)
{
    bool tx = atomic_load(&s_tx_subscribed) != 0u;
    ESP_LOGI(TAG, "BLECLI_SUBSCRIBE gen=%" PRIu32 " tx=%d active=%d", gen, tx ? 1 : 0, (tx && s_was_secure) ? 1 : 0);
}

static void on_host_evt(const ble_host_evt_t *e, void *user)
{
    (void)user;
    switch (e->type) {
    case BLE_HOST_EVT_CONNECT:
        cli_line_asm_reset(&s_asm, e->gen);
        atomic_store(&s_tx_subscribed, 0u);
        cli_latch_clear(&s_latch);
        s_link_gen = e->gen;
        s_was_secure = false;
        break;

    case BLE_HOST_EVT_SUBSCRIBE:
        if (e->attr_handle != s_tx_val_handle) {
            break; /* Service Changed, OTA TX or anything else is not ours (ADV-BLE-07) */
        }
        if (e->notify) {
            atomic_store(&s_tx_subscribed, 1u);
            if (e->secure && e->sub_reason == BLE_HOST_SUB_WRITE) {
                /* only a secure CCCD write engages; a RESTORE (or an insecure peer) never does */
                ble_host_note_engaged(s_client_id, e->gen);
            }
        } else {
            atomic_store(&s_tx_subscribed, 0u);
        }
        log_subscribe(e->gen);
        break;

    case BLE_HOST_EVT_ENC_CHANGE: {
        const bool was = s_was_secure;
        s_was_secure = e->secure;
        if (e->secure) {
            s_link_gen = e->gen;
            if (!was && atomic_load(&s_tx_subscribed) != 0u) {
                log_subscribe(e->gen); /* active changed */
            }
        } else if (was) {
            link_lost("enc_lost", s_link_gen, true); /* generation bumped by the host (REQ-FUNC-64) */
            s_link_gen = e->gen;
            atomic_store(&s_tx_subscribed, 0u);
        }
        break;
    }

    case BLE_HOST_EVT_DISCONNECT:
        snprintf(s_reason_buf, sizeof(s_reason_buf), "%d", e->status);
        link_lost(s_reason_buf, s_link_gen, s_was_secure);
        s_link_gen = 0;
        s_was_secure = false;
        atomic_store(&s_tx_subscribed, 0u);
        break;

    case BLE_HOST_EVT_HOST_RESET:
        if (s_link_gen != 0u) {
            link_lost("host_reset", s_link_gen, s_was_secure);
        }
        s_link_gen = 0;
        s_was_secure = false;
        atomic_store(&s_tx_subscribed, 0u);
        break;

    default:
        break;
    }
}

static void resmon_provider(uint32_t *cb_max_us, uint32_t *ota_rx_max_us)
{
    *cb_max_us = ble_host_cb_max_us();
    *ota_rx_max_us = ble_host_ota_rx_max_us();
}

esp_err_t ble_cli_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cmd_q = xQueueCreateStatic(CONFIG_BLE_CLI_CMD_QUEUE_DEPTH, sizeof(cmd_item_t), s_cmd_q_store, &s_cmd_q_buf);
    s_tx_q = xQueueCreateStatic(CONFIG_BLE_CLI_TX_QUEUE_DEPTH, sizeof(tx_item_t), s_tx_q_store, &s_tx_q_buf);
    if (s_cmd_q == NULL || s_tx_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cli_line_asm_reset(&s_asm, 0);
    cli_latch_init(&s_latch);
    cli_resync_init(&s_resync);

    /* The worker's stack comes from the heap once, at init (static DRAM headroom
     * is small, tech_spec.md 2.9). It is never deleted afterwards. */
    BaseType_t ok = xTaskCreatePinnedToCore(worker_fn, "blecli_wrk", CONFIG_BLE_CLI_WORKER_STACK, NULL,
                                            CONFIG_BLE_CLI_WORKER_PRIO, &s_worker, tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_worker = NULL;
        return ESP_ERR_NO_MEM;
    }

    const ble_host_client_cfg_t cfg = {
        .name = "cli",
        .svcs = s_gatt_svcs,
        .adv_uuid128 = s_adv_uuid,
        .adv_policy = BLE_HOST_ADV_ALWAYS,
        .default_primary = true,
        .cb = on_host_evt,
        .user = NULL,
        .waive_timing = false,
    };
    esp_err_t err = ble_host_register_client(&cfg, &s_client_id);
    if (err != ESP_OK) {
        vTaskDelete(s_worker); /* clean exit: the worker was never used */
        s_worker = NULL;
        return err;
    }
    diag_resmon_set_provider(resmon_provider);
    s_inited = true;
    return ESP_OK;
}
