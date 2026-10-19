/**
 * @file ble_ota_proto.c
 * @brief L2: protocol state machine, reassembly-buffer arbitration, coverage bitmap
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
 * No BLE-stack header and no esp_ota_* or esp_partition_* call anywhere in
 * this file (NFR-403) -- all flash work is requested from L3 via
 * ble_ota_flash.h's event queue and reported back via a callback.
 */
#include "ble_ota_proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ble_ota_flash.h"
#include "ble_ota_health.h"
#include "ble_ota_internal.h"
#include "ble_ota_proto_defs.h"

static const char *TAG = BLE_OTA_TAG_PROTO;

typedef enum {
    ST_IDLE = 0,
    ST_SIZE_SET,
    ST_PARAMS_SET,
    ST_RECEIVING,
    ST_COMMITTING,
    ST_INSTALLING,
    ST_RESULT_SENT,
    ST_ABORTING,
    ST_ABORTING_DRAIN,
} proto_state_t;

static const char *state_name(proto_state_t s)
{
    switch (s) {
    case ST_IDLE:            return "IDLE";
    case ST_SIZE_SET:        return "SIZE_SET";
    case ST_PARAMS_SET:      return "PARAMS_SET";
    case ST_RECEIVING:       return "RECEIVING";
    case ST_COMMITTING:      return "COMMITTING";
    case ST_INSTALLING:      return "INSTALLING";
    case ST_RESULT_SENT:     return "RESULT_SENT";
    case ST_ABORTING:        return "ABORTING";
    case ST_ABORTING_DRAIN:  return "ABORTING_DRAIN";
    default:                 return "?";
    }
}

/* tech_spec.md §10.4(1): one FWUPG_STATE line per state transition
 * actually taken, for all nine §6.1 states. A guard-refused transition
 * (the opcode/ownership check in ble_ota_proto_rx() failing) is a
 * different, already-covered case -- it never reaches here because
 * s_state never changes; it is logged via FWUPG_RX_REJECT instead. */
static void log_state_transition(proto_state_t from, proto_state_t to, const char *cause)
{
    ESP_LOGI(TAG, "FWUPG_STATE from=%s to=%s cause=%s", state_name(from), state_name(to), cause ? cause : "?");
}

typedef enum { OWNER_BLE = 0, OWNER_WORKER } buf_owner_t;

/* ---- synchronization primitives (static allocation, NFR-105) ---- */
static StaticSemaphore_t s_state_mtx_storage;
static SemaphoreHandle_t s_state_mtx;
static StaticSemaphore_t s_buf_released_storage;
static SemaphoreHandle_t s_buf_released;

/* tech_spec.md §6.7.2 (r3): the Round-4 handoff token that used to live
 * here (s_xfer_cb_ready, pairing a suspend callback's completion against
 * a later restore callback) is withdrawn along with the restore callback
 * it existed to order against. With no restore event left to race, there
 * is nothing left for a handoff token to enforce -- see
 * ble_ota_proto_set_wifi_suspend_cb() in ble_ota_proto.h for the
 * replacement (unpaired) hook. */

/* ---- L2 state, all static file-scope (NFR-404) ---- */
static proto_state_t   s_state = ST_IDLE;
static buf_owner_t     s_buffer_owner = OWNER_BLE;
static volatile bool   s_abort_requested = false;
static bool            s_drain_notified = false;
static int64_t         s_drain_enter_us = 0;

static ble_ota_notify_fn       s_notify_fn;
static ble_ota_link_ctl_fn     s_link_ctl_fn;
static ble_ota_wifi_suspend_fn s_wifi_suspend_cb;

static uint32_t s_expected_total_size;
static uint16_t s_expected_part_count;
static uint16_t s_chunk_size;
static uint32_t s_bytes_received_total;
static int32_t  s_last_committed_part = -1;

/* tech_spec.md §6.7.2 (r3): 0xEF (FORMAT_STORAGE) is wire-legal while
 * ST_RECEIVING (tech_spec.md §6.1, opcode_legal_in_state()), a 4th way to
 * leave an active transfer besides success/finalize-fail/abort. Under r2
 * this needed tracking (s_format_ends_xfer) so the matching restore
 * callback could be fired; under r3 there is no restore callback to fire,
 * so that tracking is withdrawn along with it -- a mid-transfer 0xEF now
 * needs no bookkeeping beyond the ordinary state transition itself.
 *
 * Current-part reassembly accounting. The byte buffer itself is L3's
 * (ble_ota_flash_get_part_buffer()); the bitmap/counters below are the
 * protocol-level interpretation of it (tech_spec.md §5.3/§5.4). */
static uint8_t  s_coverage[32];       /* 256 bits, one per possible chunkIndex */
static uint16_t s_chunk_len[256];     /* per-chunkIndex length, for dup-mismatch detection */
static uint16_t s_chunks_seen;
static uint32_t s_bytes_seen;

static int64_t s_last_valid_frame_us;
static bool    s_drain_timeout_logged;
static esp_timer_handle_t s_watchdog_timer;

static bool fast_mode_enabled(void)
{
#if CONFIG_BLE_OTA_FAST_MODE
    return true;
#else
    return false;
#endif
}

/* ---- notification composition (frame bytes only; L1 transmits) ---- */

/* tech_spec.md §10.4(3): one FWUPG_NOTIFY line per notification, for all
 * five types (0xEF/0xAA/0xF1/0xF2/0x0F) -- every caller below funnels
 * through this single choke point, so logging it here covers all five
 * uniformly. result= distinguishes "composed" from "actually delivered",
 * since §6.6's bounded retry means a send can still fail after this point. */
static void notify_raw(const uint8_t *frame, size_t len)
{
    uint8_t op = (len > 0) ? frame[0] : 0;
    if (s_notify_fn == NULL) {
        ESP_LOGW(TAG, "FWUPG_NOTIFY op=0x%02x len=%u result=fail:%s",
                 op, (unsigned)len, esp_err_to_name(ESP_ERR_INVALID_STATE));
        return;
    }
    esp_err_t err = s_notify_fn(frame, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FWUPG_NOTIFY op=0x%02x len=%u result=fail:%s", op, (unsigned)len, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "FWUPG_NOTIFY op=0x%02x len=%u result=ok", op, (unsigned)len);
}

static void send_mode_ack(uint8_t mode)
{
    uint8_t f[2] = { BLE_OTA_NOTIFY_MODE_ACK, mode };
    notify_raw(f, sizeof(f));
}

static void send_storage_size(uint32_t total_bytes, uint32_t used_bytes)
{
    uint8_t f[7];
    f[0] = BLE_OTA_NOTIFY_STORAGE_SIZE;
    f[1] = (uint8_t)(total_bytes >> 16);
    f[2] = (uint8_t)(total_bytes >> 8);
    f[3] = (uint8_t)(total_bytes);
    f[4] = (uint8_t)(used_bytes >> 16);
    f[5] = (uint8_t)(used_bytes >> 8);
    f[6] = (uint8_t)(used_bytes);
    notify_raw(f, sizeof(f));
}

static void send_req_next_part(uint16_t idx)
{
    uint8_t f[3] = { BLE_OTA_NOTIFY_REQ_NEXT_PART, (uint8_t)(idx >> 8), (uint8_t)idx };
    notify_raw(f, sizeof(f));
}

static void send_xfer_complete(uint16_t idx)
{
    uint8_t f[3] = { BLE_OTA_NOTIFY_XFER_COMPLETE, (uint8_t)(idx >> 8), (uint8_t)idx };
    notify_raw(f, sizeof(f));
}

static void send_ota_result(const char *text)
{
    uint8_t f[BLE_OTA_NOTIFY_MAX_LEN];
    f[0] = BLE_OTA_NOTIFY_OTA_RESULT;
    size_t max_payload = sizeof(f) - 1;
    size_t n = strnlen(text, max_payload);
    memcpy(&f[1], text, n);
    notify_raw(f, 1 + n);
}

static void send_reject_notify(const char *text)
{
#if CONFIG_BLE_OTA_REPORT_REJECT_VIA_0X0F
    send_ota_result(text);
#else
    (void)text;
#endif
}

/* ---- opcode legality table (tech_spec.md §6.1) ---- */

static bool opcode_legal_in_state(uint8_t opcode, proto_state_t st)
{
    switch (st) {
    case ST_IDLE:
        return opcode == BLE_OTA_OP_SET_FILE_SIZE || opcode == BLE_OTA_OP_FORMAT_STORAGE;
    case ST_SIZE_SET:
        return opcode == BLE_OTA_OP_SET_XFER_PARAMS || opcode == BLE_OTA_OP_SET_FILE_SIZE ||
               opcode == BLE_OTA_OP_FORMAT_STORAGE;
    case ST_PARAMS_SET:
        return opcode == BLE_OTA_OP_START_TRANSFER || opcode == BLE_OTA_OP_SET_FILE_SIZE ||
               opcode == BLE_OTA_OP_SET_XFER_PARAMS || opcode == BLE_OTA_OP_FORMAT_STORAGE;
    case ST_RECEIVING:
        return opcode == BLE_OTA_OP_DATA_CHUNK || opcode == BLE_OTA_OP_END_OF_PART ||
               opcode == BLE_OTA_OP_FORMAT_STORAGE;
    default:
        /* COMMITTING, INSTALLING, RESULT_SENT, ABORTING, ABORTING_DRAIN:
         * empty legal set (BUF-INV's non-accepting states). */
        return false;
    }
}

/* ---- big-endian field readers ---- */
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ---- per-part transient state reset (tech_spec.md §5.4 "on part start") ---- */
static void reset_part_transient_state(void)
{
    uint8_t *buf = ble_ota_flash_get_part_buffer();
    memset(buf, 0, ble_ota_flash_get_part_buffer_size());
    memset(s_coverage, 0, sizeof(s_coverage));
    memset(s_chunk_len, 0, sizeof(s_chunk_len));
    s_chunks_seen = 0;
    s_bytes_seen = 0;
}

/* Not a macro: comparing a uint16_t against the literal 65536 (the
 * default CONFIG_BLE_OTA_MAX_PART_SIZE) is provably always-false at that
 * default and trips -Wtype-limits; routing the bound through a
 * non-constant object keeps the check meaningful for any configured
 * value (range 4096-65536) without a spurious warning at the default. */
static const uint32_t k_max_part_size = (uint32_t)CONFIG_BLE_OTA_MAX_PART_SIZE;

static inline bool coverage_get(uint16_t idx) { return (s_coverage[idx >> 3] & (1u << (idx & 7))) != 0; }
static inline void coverage_set(uint16_t idx) { s_coverage[idx >> 3] |= (uint8_t)(1u << (idx & 7)); }

/* ---- reboot sequencing (tech_spec.md §7.4) ---- */

static void reboot_task_fn(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BLE_OTA_REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "FWUPG_REBOOT");
    esp_restart();
}

static void schedule_reboot(void)
{
    if (s_link_ctl_fn != NULL) {
        s_link_ctl_fn(BLE_OTA_LINK_CTL_DISCONNECT);
    }
    TaskHandle_t t = NULL;
    BaseType_t ok = xTaskCreate(reboot_task_fn, "ble_ota_reboot", 2048, NULL, 3, &t);
    if (ok != pdPASS) {
        /* Extremely unlikely (task-create failure); fall back to an
         * immediate restart rather than never rebooting at all. */
        ESP_LOGE(TAG, "reboot task create failed; restarting immediately");
        esp_restart();
    }
}

/* ---- abort, locked-body variant (Phase 5 review CRITICAL-1 fix) ----
 *
 * s_state_mtx is a plain (non-recursive) FreeRTOS mutex. The RX opcode
 * handlers below run with it ALREADY held by ble_ota_proto_rx(), so they
 * must never call the public ble_ota_proto_abort() (which itself takes
 * s_state_mtx) -- doing so previously self-deadlocked the calling task
 * for the full 100 ms take-timeout and then silently no-op'd (no state
 * change, no EVT_ABORT posted, no FWUPG_ABORT log line), because the
 * early "if take failed, return ESP_ERR_TIMEOUT" guard fired every time.
 * abort_locked() is the same body WITHOUT the take/give, for callers that
 * already hold the lock; ble_ota_proto_abort() (below) is the entry point
 * for every caller that does NOT already hold it (fwupgrade stop, BLE
 * disconnect/encryption-loss, the inactivity-timeout watchdog, and
 * flash_done_cb()'s failure paths -- which already correctly release
 * s_state_mtx before calling ble_ota_proto_abort(); that pattern is
 * unchanged). */
static esp_err_t abort_locked(const char *reason)
{
    if (s_abort_requested) {
        return ESP_OK; /* idempotent (FR-706) */
    }
    if (s_state == ST_IDLE) {
        /* Nothing to abort; still perfectly valid to call. */
        return ESP_OK;
    }

    proto_state_t from = s_state;
    s_abort_requested = true;
    ESP_LOGW(TAG, "FWUPG_ABORT reason=%s", reason ? reason : "unspecified");

    ble_ota_flash_evt_t evt = { .type = BLE_OTA_FLASH_EVT_ABORT };
    (void)ble_ota_flash_post_event(&evt, 0);

    if (s_buffer_owner == OWNER_BLE) {
        s_state = ST_ABORTING;
    } else {
        s_state = ST_ABORTING_DRAIN;
        s_drain_notified = false;
        s_drain_enter_us = esp_timer_get_time();
        ESP_LOGW(TAG, "FWUPG_DRAIN_BEGIN");
    }
    log_state_transition(from, s_state, reason ? reason : "unspecified");

    return ESP_OK;
}

/* ---- RX opcode handlers (mutex already held by the caller) ---- */

static void handle_set_file_size(const uint8_t *frame)
{
    uint32_t size = be32(&frame[1]);

    if (size == 0 || size > BLE_OTA_MAX_IMAGE_SIZE) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFE state=%s reason=size_too_large size=%" PRIu32,
                 state_name(s_state), size);
        char msg[BLE_OTA_NOTIFY_MAX_LEN];
        snprintf(msg, sizeof(msg), "ERR size %" PRIu32, size);
        send_reject_notify(msg);
        return;
    }

    if (ble_ota_health_is_pending_verify()) {
        /* Finding F-2 defence in depth (tech_spec.md §6.4(2)): unreachable
         * in the normal flow since fwupgrade start already refuses, but
         * kept as the app-facing surface. */
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFE state=%s reason=pending_verify", state_name(s_state));
        send_reject_notify("ERR pending_verify");
        return;
    }

    proto_state_t from = s_state;
    s_expected_total_size = size;
    s_bytes_received_total = 0;
    s_state = ST_SIZE_SET;
    log_state_transition(from, s_state, "0xFE");
}

static void handle_set_xfer_params(const uint8_t *frame)
{
    uint16_t parts = be16(&frame[1]);
    uint16_t chunk_sz = be16(&frame[3]);

    if (parts < 1) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFF state=%s reason=zero_parts", state_name(s_state));
        send_reject_notify("ERR parts=0");
        return;
    }
    if (chunk_sz < 1 || chunk_sz > BLE_OTA_MAX_CHUNK_SIZE) {
        /* FR-411: rejected before entering RECEIVING, never clamped. */
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFF state=%s reason=chunk_too_big chunk=%u max=%u",
                 state_name(s_state), chunk_sz, (unsigned)BLE_OTA_MAX_CHUNK_SIZE);
        char msg[BLE_OTA_NOTIFY_MAX_LEN];
        snprintf(msg, sizeof(msg), "ERR chunk %u>%u", chunk_sz, (unsigned)BLE_OTA_MAX_CHUNK_SIZE);
        send_reject_notify(msg);
        return;
    }
    if ((uint64_t)parts * (uint64_t)BLE_OTA_MAX_PART_SIZE < (uint64_t)s_expected_total_size) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFF state=%s reason=too_few_parts", state_name(s_state));
        send_reject_notify("ERR parts too few");
        return;
    }
    /* Phase 5 review CRITICAL (chunk-count overflow, FR-405/FR-411): a
     * chunkSize small enough that a maximal-size part would need more
     * than 256 chunks would let handle_end_of_part()'s coverage-bitmap
     * checks index s_coverage (a fixed 256-bit array) out of bounds.
     * Reject before entering PARAMS_SET, never clamped -- same pattern as
     * the oversized-chunkSize case above. */
    if (((uint32_t)BLE_OTA_MAX_PART_SIZE + chunk_sz - 1) / chunk_sz > 256) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFF state=%s reason=chunk_too_small chunk=%u",
                 state_name(s_state), chunk_sz);
        char msg[BLE_OTA_NOTIFY_MAX_LEN];
        snprintf(msg, sizeof(msg), "ERR chunk %u<min", chunk_sz);
        send_reject_notify(msg);
        return;
    }

    proto_state_t from = s_state;
    s_expected_part_count = parts;
    s_chunk_size = chunk_sz;
    s_state = ST_PARAMS_SET;
    log_state_transition(from, s_state, "0xFF");
}

/* Sets the state machine into an active transfer (ST_RECEIVING) and posts
 * EVT_BEGIN to the flash worker, both synchronously under s_state_mtx --
 * the same pattern every other RX handler in this file uses for its own
 * flash-event post (handle_end_of_part(), handle_format_storage()).
 *
 * tech_spec.md §6.7.2 (r3): under r2 this used to defer both the Wi-Fi
 * suspend callback invocation *and* the EVT_BEGIN post to the caller,
 * specifically to order "suspend callback completes" before "worker can
 * observe EVT_BEGIN" -- because a restore callback existed downstream
 * that needed that ordering to avoid firing before its matching suspend
 * had completed (Round 3 MAJOR-3). r3 withdraws the restore callback
 * entirely, so there is no longer anything for that ordering to protect:
 * EVT_BEGIN can go back to being posted here, under the lock, like every
 * other event in this file. The one remaining external call --
 * ble_ota_proto_rx()'s invocation of the Wi-Fi suspend hook -- still must
 * happen only after s_state_mtx is released (NFR-106: the hook may block
 * on esp_wifi_* calls), so entering ST_RECEIVING still reports back to
 * the caller via the return value below, but nothing about EVT_BEGIN
 * depends on that hook anymore.
 *
 * Returns true iff the transfer genuinely began (EVT_BEGIN was
 * successfully queued); false if it could not be (queue full), in which
 * case this already aborted and there is nothing for the caller to
 * suspend Wi-Fi for. */
static bool handle_start_transfer(void)
{
    proto_state_t from = s_state;
    reset_part_transient_state();
    s_last_committed_part = -1;
    s_bytes_received_total = 0;

    uint8_t mode = fast_mode_enabled() ? 1 : 0;
    s_state = ST_RECEIVING;
    send_mode_ack(mode);
    /* tech_spec.md §6.7.1: PARAMS_SET -> RECEIVING is the Wi-Fi-suspend
     * trigger point. This FWUPG_STATE line and the FWUPG_WIFI_SUSPEND_WARN
     * / FWUPG_WIFI_SUSPENDED pair the caller emits after this returns are
     * deliberately separate lines (§10.4(1)). */
    log_state_transition(from, s_state, "0xFD");

    ble_ota_flash_evt_t evt = {
        .type = BLE_OTA_FLASH_EVT_BEGIN,
        .len = s_expected_total_size,
        .count = s_expected_part_count,
    };
    esp_err_t err = ble_ota_flash_post_event(&evt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFD state=%s reason=queue_full", state_name(s_state));
        abort_locked("queue_full_begin"); /* s_state_mtx already held (Phase 5 review CRITICAL-1) */
        send_reject_notify("ERR busy");
        return false;
    }

    return true;
}

static void handle_data_chunk(const uint8_t *frame, size_t len)
{
    uint8_t chunk_index = frame[1];
    const uint8_t *payload = &frame[2];
    size_t n = len - 2;

    if (n < 1 || n > s_chunk_size) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFB state=%s reason=bad_len n=%u", state_name(s_state), (unsigned)n);
        return; /* per-frame rejection on a live transfer: log only (§8) */
    }

    uint32_t offset = (uint32_t)chunk_index * s_chunk_size;
    if (offset + n > BLE_OTA_MAX_PART_SIZE) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0xFB state=%s reason=out_of_bounds idx=%u", state_name(s_state), chunk_index);
        return;
    }

    if (coverage_get(chunk_index)) {
        if (s_chunk_len[chunk_index] == n) {
            /* Idempotent retransmission -- normal on BLE. */
            ESP_LOGD(TAG, "dup chunk idx=%u len=%u (idempotent)", chunk_index, (unsigned)n);
        } else {
            ESP_LOGE(TAG, "dup chunk idx=%u len mismatch (%u vs %u)", chunk_index,
                      (unsigned)n, s_chunk_len[chunk_index]);
            abort_locked("dup_chunk_mismatch"); /* s_state_mtx already held (Phase 5 review CRITICAL-1) */
            send_reject_notify("ERR dup chunk");
            return;
        }
    } else {
        s_chunks_seen++;
        s_bytes_seen += n;
        coverage_set(chunk_index);
        s_chunk_len[chunk_index] = (uint16_t)n;
    }

    uint8_t *buf = ble_ota_flash_get_part_buffer();
    memcpy(buf + offset, payload, n);
}

static void handle_end_of_part(const uint8_t *frame)
{
    uint16_t part_len = be16(&frame[1]);
    uint16_t part_index = be16(&frame[3]);

    if (part_len < 1 || (uint32_t)part_len > k_max_part_size) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=bad_partlen", state_name(s_state));
        abort_locked("bad_partlen"); /* s_state_mtx already held (Phase 5 review CRITICAL-1) */
        send_reject_notify("ERR partlen");
        return;
    }
    if (part_index != (uint16_t)(s_last_committed_part + 1)) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=order idx=%u expected=%d",
                  state_name(s_state), part_index, s_last_committed_part + 1);
        abort_locked("part_order");
        send_reject_notify("ERR part order");
        return;
    }

    uint32_t expected_chunks32 = ((uint32_t)part_len + s_chunk_size - 1) / s_chunk_size;
    if (expected_chunks32 > 256) {
        /* Defence in depth (Phase 5 review CRITICAL-1/FR-405): handle_set_xfer_params()
         * now rejects a chunkSize that would let this happen, but never index
         * the 256-bit s_coverage bitmap out of bounds regardless of that
         * upstream check. */
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=chunk_count_overflow part=%u count=%" PRIu32,
                  state_name(s_state), part_index, expected_chunks32);
        abort_locked("chunk_count_overflow");
        send_reject_notify("ERR chunk count");
        return;
    }
    uint16_t expected_chunks = (uint16_t)expected_chunks32;

    for (uint16_t i = 0; i < expected_chunks; i++) {
        if (!coverage_get(i)) {
            ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=chunk_gap part=%u missing=%u",
                      state_name(s_state), part_index, i);
            abort_locked("chunk_gap");
            send_reject_notify("ERR gap");
            return;
        }
    }
    for (uint16_t i = expected_chunks; i < 256; i++) {
        if (coverage_get(i)) {
            ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=extra_chunk part=%u idx=%u",
                      state_name(s_state), part_index, i);
            abort_locked("extra_chunk");
            send_reject_notify("ERR extra chunk");
            return;
        }
    }

    uint16_t last_expected_len = (uint16_t)(part_len - (uint32_t)(expected_chunks - 1) * s_chunk_size);
    if (s_bytes_seen != part_len || s_chunk_len[expected_chunks - 1] != last_expected_len) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=len_mismatch part=%u seen=%" PRIu32 " declared=%u",
                  state_name(s_state), part_index, s_bytes_seen, part_len);
        abort_locked("part_len_mismatch");
        send_reject_notify("ERR len");
        return;
    }

    ble_ota_flash_evt_t evt = {
        .type = BLE_OTA_FLASH_EVT_COMMIT_PART,
        .len = part_len,
        .count = part_index,
    };
    esp_err_t err = ble_ota_flash_post_event(&evt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xFC state=%s reason=queue_full", state_name(s_state));
        abort_locked("queue_full");
        send_reject_notify("ERR busy");
        return;
    }

    proto_state_t from = s_state;
    s_buffer_owner = OWNER_WORKER;
    s_state = ST_COMMITTING;
    log_state_transition(from, s_state, "0xFC");
}

static void handle_format_storage(void)
{
    ble_ota_flash_evt_t evt = { .type = BLE_OTA_FLASH_EVT_FORMAT };
    esp_err_t err = ble_ota_flash_post_event(&evt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FWUPG_RX_REJECT op=0xEF state=%s reason=queue_full", state_name(s_state));
        send_reject_notify("ERR busy");
        return;
    }
    /* Format touches only the inactive slot and never the reassembly
     * buffer's *content* meaningfully (no transfer in progress can
     * coexist with a legal 0xEF -- BUF-INV already guarantees
     * buffer_owner==OWNER_BLE here), but is still routed through the
     * same ownership handoff as a commit, uniformly, per tech_spec.md
     * §5.9.4's parenthetical ("and for 0xEF the bounded
     * esp_partition_erase_range() loop").
     *
     * tech_spec.md §6.7.2 (r3): 0xEF is wire-legal from ST_RECEIVING too,
     * so this can end an in-progress transfer. Under r2 that case needed
     * tracking so the matching Wi-Fi-restore callback could be fired once
     * formatting completed; r3 has no restore callback, so entering from
     * ST_RECEIVING needs no special handling beyond the ordinary state
     * transition logged below -- there is nothing left to fire. */
    proto_state_t from = s_state;
    s_buffer_owner = OWNER_WORKER;
    s_last_committed_part = -1;
    s_bytes_received_total = 0;
    s_state = ST_COMMITTING;
    log_state_transition(from, s_state, "0xEF");
}

/* ---- public entry points ---- */

void ble_ota_proto_rx(bool link_secure, const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0) {
        return;
    }
    uint8_t opcode = frame[0];

    if (!link_secure) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0x%02x state=%s reason=not_bonded", opcode, state_name(s_state));
        return; /* no notification at all (FR-301/FR-302/§8) */
    }

    const ble_ota_rx_frame_desc_t *desc = ble_ota_rx_frame_desc_find(opcode);
    if (desc == NULL) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0x%02x state=%s reason=unknown_opcode", opcode, state_name(s_state));
        send_reject_notify("ERR opcode");
        return;
    }
    if (desc->fixed_len != 0) {
        if (len != desc->fixed_len) {
            ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0x%02x state=%s reason=bad_length len=%u",
                     opcode, state_name(s_state), (unsigned)len);
            send_reject_notify("ERR length");
            return;
        }
    } else if (len < desc->min_len) {
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0x%02x state=%s reason=bad_length len=%u",
                 opcode, state_name(s_state), (unsigned)len);
        send_reject_notify("ERR length");
        return;
    }

    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "state mutex timeout processing op=0x%02x", opcode);
        return;
    }

    if (!opcode_legal_in_state(opcode, s_state) || s_buffer_owner != OWNER_BLE) {
        bool draining = (s_state == ST_ABORTING_DRAIN);
        ESP_LOGW(TAG, "FWUPG_RX_REJECT op=0x%02x state=%s reason=%s",
                 opcode, state_name(s_state), draining ? "busy_draining" : "sequence");
        bool notify_once = draining && !s_drain_notified;
        if (notify_once) {
            s_drain_notified = true;
        }
        xSemaphoreGive(s_state_mtx);
        if (notify_once) {
            send_reject_notify("ERR busy retry");
        } else if (!draining) {
            send_reject_notify("ERR sequence");
        }
        return;
    }

    s_last_valid_frame_us = esp_timer_get_time();

    /* tech_spec.md §10.4(2): one FWUPG_RX line per *accepted* frame, for
     * every opcode -- 0xFB (data chunk) is the level-only exception
     * (DEBUG, compiled out of the shipping build, per §4.2.1/FR-1003:
     * a 64 KiB part at a 512-byte chunk size is up to 128 frames and
     * FR-1003 forbids flooding INFO). No argument here has a side effect. */
    if (opcode == BLE_OTA_OP_DATA_CHUNK) {
        ESP_LOGD(TAG, "FWUPG_RX op=0x%02x state=%s len=%u", opcode, state_name(s_state), (unsigned)len);
    } else {
        ESP_LOGI(TAG, "FWUPG_RX op=0x%02x state=%s len=%u", opcode, state_name(s_state), (unsigned)len);
    }

    bool xfer_began = false;

    switch (opcode) {
    case BLE_OTA_OP_SET_FILE_SIZE:   handle_set_file_size(frame); break;
    case BLE_OTA_OP_SET_XFER_PARAMS: handle_set_xfer_params(frame); break;
    case BLE_OTA_OP_START_TRANSFER:  xfer_began = handle_start_transfer(); break;
    case BLE_OTA_OP_DATA_CHUNK:      handle_data_chunk(frame, len); break;
    case BLE_OTA_OP_END_OF_PART:     handle_end_of_part(frame); break;
    case BLE_OTA_OP_FORMAT_STORAGE:  handle_format_storage(); break;
    default: break; /* unreachable: desc lookup above already validated opcode */
    }

    xSemaphoreGive(s_state_mtx);

    /* tech_spec.md §6.7.1/§6.7.2 (r3): the Wi-Fi suspend hook is invoked
     * only after s_state_mtx is released -- it may block on esp_wifi_*
     * calls, and NFR-106 forbids holding this mutex across a call this
     * layer does not control the latency of. Unlike r2's paired
     * suspend/restore callback, there is no ordering requirement left to
     * satisfy here: EVT_BEGIN was already posted synchronously inside
     * handle_start_transfer() above, under the lock, exactly like every
     * other flash event in this file -- nothing downstream depends on
     * this hook's completion, so it is simply called and forgotten. */
    if (xfer_began && s_wifi_suspend_cb != NULL) {
        s_wifi_suspend_cb();
    }
}

void ble_ota_proto_on_connect(void)
{
    size_t slot_size = 0;
    if (ble_ota_flash_get_target_slot(NULL, &slot_size) == ESP_OK) {
        send_storage_size((uint32_t)slot_size, ble_ota_flash_get_used_bytes());
    }
}

void ble_ota_proto_on_disconnect(void)
{
    (void)ble_ota_proto_abort("disconnect");
}

esp_err_t ble_ota_proto_abort(const char *reason)
{
    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = abort_locked(reason);
    xSemaphoreGive(s_state_mtx);
    return err;
}

bool ble_ota_proto_wait_idle(uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool idle = (s_state == ST_IDLE);
            xSemaphoreGive(s_state_mtx);
            if (idle) {
                return true;
            }
        }
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ble_ota_proto_set_wifi_suspend_cb(ble_ota_wifi_suspend_fn cb)
{
    s_wifi_suspend_cb = cb;
}

const char *ble_ota_proto_state_name(void)
{
    proto_state_t st = s_state;
    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        st = s_state;
        xSemaphoreGive(s_state_mtx);
    }
    return state_name(st);
}

void ble_ota_proto_get_progress(uint32_t *bytes_received, uint32_t *bytes_declared)
{
    uint32_t recv = s_bytes_received_total;
    uint32_t declared = s_expected_total_size;
    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        recv = s_bytes_received_total;
        declared = s_expected_total_size;
        xSemaphoreGive(s_state_mtx);
    }
    if (bytes_received != NULL) {
        *bytes_received = recv;
    }
    if (bytes_declared != NULL) {
        *bytes_declared = declared;
    }
}

bool ble_ota_proto_is_drain_stuck(void)
{
    proto_state_t st;
    int64_t enter_us;
    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false; /* transient: treat "couldn't confirm" as "not stuck" */
    }
    st = s_state;
    enter_us = s_drain_enter_us;
    xSemaphoreGive(s_state_mtx);

    if (st != ST_ABORTING_DRAIN) {
        return false;
    }
    int64_t elapsed_ms = (esp_timer_get_time() - enter_us) / 1000;
    return elapsed_ms > CONFIG_BLE_OTA_DRAIN_TIMEOUT_MS;
}

/* ---- flash completion callback (runs on ble_ota_wrk, §5.9.4) ---- */

static bool abort_query_cb(void)
{
    return s_abort_requested;
}

static void flash_done_cb(const ble_ota_flash_result_t *r)
{
    /* Set at most once below, acted on AFTER s_state_mtx is released --
     * tech_spec.md §3.4/NFR-106 forbid holding this mutex across a BLE API
     * call (schedule_reboot() -> s_link_ctl_fn() -> ble_gap_terminate()).
     * tech_spec.md §6.7.2 (r3): there used to be a second deferred flag
     * here (xfer_ended) driving a transfer-end Wi-Fi-restore callback;
     * that callback is withdrawn along with everything that ordered
     * against it, so only the reboot deferral remains. */
    bool do_reboot = false;

    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "flash_done_cb: state mutex timeout (kind=%d)", (int)r->kind);
        return;
    }

    /* Step 1-2 of §5.9.4's worker path: release ownership and signal it,
     * unconditionally, before anything else. */
    s_buffer_owner = OWNER_BLE;
    xSemaphoreGive(s_buf_released);

    if (r->kind == BLE_OTA_FLASH_RESULT_ABORTED) {
        proto_state_t from = s_state;
        reset_part_transient_state();
        s_last_committed_part = -1;
        s_bytes_received_total = 0;
        (void)xSemaphoreTake(s_buf_released, 0); /* consume the token just given above */
        bool was_draining = (s_state == ST_ABORTING_DRAIN);
        s_state = ST_IDLE;
        s_abort_requested = false;
        if (was_draining) {
            ESP_LOGW(TAG, "FWUPG_DRAIN_END");
        }
        log_state_transition(from, s_state, "abort_complete");
        xSemaphoreGive(s_state_mtx);
        send_reject_notify("ERR aborted");
        return;
    }

    if (s_abort_requested) {
        /* Belongs to an operation already in flight when the abort was
         * requested; its own progress logic is skipped. The EVT_ABORT
         * queued behind it (never overtaken) runs next. */
        xSemaphoreGive(s_state_mtx);
        return;
    }

    switch (r->kind) {
    case BLE_OTA_FLASH_RESULT_BEGIN_OK:
        break; /* already in RECEIVING; 0xAA already sent synchronously at 0xFD */

    case BLE_OTA_FLASH_RESULT_PART_OK: {
        proto_state_t from = s_state;
        s_last_committed_part = (int32_t)r->part_index;
        s_bytes_received_total = r->bytes_committed;
        reset_part_transient_state();
        uint16_t next = (uint16_t)(r->part_index + 1);
        uint32_t pct = (s_expected_total_size != 0)
                           ? (uint32_t)((uint64_t)s_bytes_received_total * 100u / s_expected_total_size)
                           : 0u;
        ESP_LOGI(TAG, "FWUPG_PROGRESS pct=%" PRIu32 " bytes=%" PRIu32 "/%" PRIu32,
                 pct, s_bytes_received_total, s_expected_total_size);
        if (next < s_expected_part_count) {
            s_state = ST_RECEIVING;
            log_state_transition(from, s_state, "part_committed");
            if (!fast_mode_enabled()) {
                send_req_next_part(next);
            }
        } else {
            s_state = ST_INSTALLING;
            log_state_transition(from, s_state, "all_parts_committed");
            send_xfer_complete(next); /* spec §4: 0xF2 sent BEFORE finalization */
            ble_ota_flash_evt_t evt = { .type = BLE_OTA_FLASH_EVT_FINALIZE, .count = next };
            (void)ble_ota_flash_post_event(&evt, 0);
        }
        break;
    }

    case BLE_OTA_FLASH_RESULT_FINALIZE_OK: {
        proto_state_t from = s_state;
        s_state = ST_RESULT_SENT;
        log_state_transition(from, s_state, "finalize_ok");
        char msg[BLE_OTA_NOTIFY_MAX_LEN];
        snprintf(msg, sizeof(msg), "OK %" PRIu32 "B", s_bytes_received_total);
        send_ota_result(msg);
        do_reboot = true;  /* schedule_reboot() called after unlock below */
        break;
    }

    case BLE_OTA_FLASH_RESULT_FORMAT_OK: {
        proto_state_t from = s_state;
        s_state = ST_IDLE;
        log_state_transition(from, s_state, "format_ok");
        size_t slot_size = 0;
        if (ble_ota_flash_get_target_slot(NULL, &slot_size) == ESP_OK) {
            send_storage_size((uint32_t)slot_size, 0);
        }
        break;
    }

    case BLE_OTA_FLASH_RESULT_BEGIN_FAIL:
    case BLE_OTA_FLASH_RESULT_PART_FAIL:
    case BLE_OTA_FLASH_RESULT_FINALIZE_FAIL:
    case BLE_OTA_FLASH_RESULT_FORMAT_FAIL: {
        const char *reason =
            (r->kind == BLE_OTA_FLASH_RESULT_BEGIN_FAIL)    ? "begin_failed" :
            (r->kind == BLE_OTA_FLASH_RESULT_PART_FAIL)     ? "flash_write_failed" :
            (r->kind == BLE_OTA_FLASH_RESULT_FINALIZE_FAIL) ? "finalize_failed" : "format_failed";
        ESP_LOGE(TAG, "FWUPG_FINALIZE_FAIL err=%s reason=%s", esp_err_to_name(r->err), reason);
        xSemaphoreGive(s_state_mtx);
        ble_ota_proto_abort(reason); /* transfer-end (finalize-failure) is signalled
                                        via the ABORTED path this triggers, above */
        send_reject_notify("ERR flash");
        return;
    }

    default:
        break;
    }

    xSemaphoreGive(s_state_mtx);

    /* Deferred until here so it does not run with s_state_mtx held
     * (Phase 5 review MAJOR-1: schedule_reboot() -> ble_gap_terminate() is
     * a BLE API call). */
    if (do_reboot) {
        schedule_reboot();
    }
}

/* ---- watchdog tick: inactivity timeout (FR-705) and drain-timeout log
 * (§5.9.6). Runs on the shared esp_timer task; each check is a few
 * comparisons and at most one ble_ota_proto_abort() call, never a flash
 * or BLE-stack call, so dispatching directly on ESP_TIMER_TASK (rather
 * than a dedicated task) does not risk starving other esp_timer users. */
static void watchdog_tick_cb(void *arg)
{
    (void)arg;

    if (ble_ota_proto_is_drain_stuck()) {
        if (!s_drain_timeout_logged) {
            ESP_LOGE(TAG, "FWUPG_DRAIN_TIMEOUT");
            s_drain_timeout_logged = true;
        }
        return; /* stays non-accepting; BUF-INV holds; recovery is a reset (§5.9.6) */
    }
    s_drain_timeout_logged = false;

    if (xSemaphoreTake(s_state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    proto_state_t st = s_state;
    int64_t last = s_last_valid_frame_us;
    xSemaphoreGive(s_state_mtx);

    if (st == ST_SIZE_SET || st == ST_PARAMS_SET || st == ST_RECEIVING) {
        int64_t idle_s = (esp_timer_get_time() - last) / 1000000;
        if (idle_s > CONFIG_BLE_OTA_INACTIVITY_TIMEOUT_S) {
            ble_ota_proto_abort("inactivity_timeout");
        }
    }
}

esp_err_t ble_ota_proto_attach(ble_ota_notify_fn notify, ble_ota_link_ctl_fn ctl)
{
    if (notify == NULL || ctl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_notify_fn = notify;
    s_link_ctl_fn = ctl;
    return ESP_OK;
}

esp_err_t ble_ota_proto_init(void)
{
    if (s_state_mtx != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state_mtx = xSemaphoreCreateMutexStatic(&s_state_mtx_storage);
    s_buf_released = xSemaphoreCreateBinaryStatic(&s_buf_released_storage);
    if (s_state_mtx == NULL || s_buf_released == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state = ST_IDLE;
    s_buffer_owner = OWNER_BLE;
    s_abort_requested = false;
    reset_part_transient_state();

    esp_err_t err = ble_ota_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    err = ble_ota_flash_attach(flash_done_cb, abort_query_cb);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t targs = {
        .callback = watchdog_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_ota_proto_wd",
    };
    err = esp_timer_create(&targs, &s_watchdog_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_watchdog_timer, 5000000ULL);
}
