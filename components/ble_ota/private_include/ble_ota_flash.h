/**
 * @file ble_ota_flash.h
 * @brief L3 interface: OTA write backend, worker task, and its completion callback
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
 * L3 -- the lowest layer (NFR-403). MUST NOT include any BLE-stack
 * header. All esp_ota_* or esp_partition_* calls live in ble_ota_flash.c
 * and nowhere else in this component.
 *
 * The per-part reassembly buffer physically lives here (it is what a
 * single esp_ota_write() call consumes) and is exposed to L2 only as an
 * opaque pointer/size pair via ble_ota_flash_get_part_buffer(); L2 owns
 * the *semantics* of what is written into it (coverage bitmap, chunk
 * placement, buffer_owner arbitration, §5.3/§5.4/§5.9) and the mutex that
 * gates who may touch it.
 *
 * Upward data (a commit result, which only L2 can interpret) flows via a
 * downward-registered callback, exactly like ble_ota_proto.h's notify_fn
 * (tech_spec.md §3.3): the TYPE is declared here (by L3, the lower
 * layer), but the function body lives in L2 and is supplied through
 * ble_ota_flash_attach(), called by L2 during its own init.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_OTA_FLASH_EVT_BEGIN = 0, /* 0xFD: open a fresh esp_ota_* write session */
    BLE_OTA_FLASH_EVT_COMMIT_PART, /* 0xFC accepted: esp_ota_write(part_buf[0:part_len]) */
    BLE_OTA_FLASH_EVT_FINALIZE,  /* all declared parts committed: validate + switch boot slot */
    BLE_OTA_FLASH_EVT_FORMAT,    /* 0xEF: erase the inactive slot in bounded steps */
    BLE_OTA_FLASH_EVT_ABORT,     /* any abort source (§5.9) */
} ble_ota_flash_evt_type_t;

/* Queue item (tech_spec.md §3.4 s_flash_evt_q). Field meaning depends on
 * `type`:
 *   BEGIN     -> len = declared 0xFE image size, count = declared 0xFF part count
 *   COMMIT_PART -> len = partLen, count = partIndex
 *   FINALIZE  -> count = final partIndex + 1 (total committed parts)
 *   FORMAT, ABORT -> unused
 */
typedef struct {
    ble_ota_flash_evt_type_t type;
    uint32_t                 len;
    uint16_t                 count;
} ble_ota_flash_evt_t;

typedef enum {
    BLE_OTA_FLASH_RESULT_BEGIN_OK = 0,
    BLE_OTA_FLASH_RESULT_BEGIN_FAIL,
    BLE_OTA_FLASH_RESULT_PART_OK,
    BLE_OTA_FLASH_RESULT_PART_FAIL,
    BLE_OTA_FLASH_RESULT_FINALIZE_OK,
    BLE_OTA_FLASH_RESULT_FINALIZE_FAIL,
    BLE_OTA_FLASH_RESULT_FORMAT_OK,
    BLE_OTA_FLASH_RESULT_FORMAT_FAIL,
    BLE_OTA_FLASH_RESULT_ABORTED,
} ble_ota_flash_result_kind_t;

typedef struct {
    ble_ota_flash_result_kind_t kind;
    esp_err_t                   err;              /* ESP_OK on the *_OK kinds */
    uint16_t                    part_index;
    uint32_t                    bytes_committed;   /* running total, for FR-407 */
} ble_ota_flash_result_t;

/* Invoked on the ble_ota_wrk task, synchronously, immediately after the
 * esp_ota_* or esp_partition_* call the event required has returned -- i.e.
 * exactly the point tech_spec.md §5.9.4 calls "the worker, on returning
 * from any call that read or wrote s_part_buf". This is where L2's
 * buffer_owner is set back to OWNER_BLE and s_buf_released is given
 * (§5.9); the callback body lives in L2 (ble_ota_proto.c). */
typedef void (*ble_ota_flash_done_fn)(const ble_ota_flash_result_t *result);

/* Queried by the worker at the top of every dequeued event (except
 * EVT_ABORT itself, which always runs): "has an abort been requested
 * since this event was queued?" Lets the worker DROP a stale
 * EVT_COMMIT_PART/EVT_BEGIN/EVT_FORMAT without touching flash
 * (tech_spec.md §3.4/§5.9.4's "the worker re-reads the sticky
 * s_abort_requested at the top of every dequeued event"). s_abort_requested
 * itself is L2 state; this is the downward-declared-callback pattern
 * (§3.3) applied so L3 never has to read an L2 variable directly. */
typedef bool (*ble_ota_flash_abort_query_fn)(void);

/* One-time init: creates s_flash_evt_q. Called once from ble_ota_init(). */
esp_err_t ble_ota_flash_init(void);

/* Registers L2's completion callback and abort-pending query. Called
 * once, from L2's own init. */
esp_err_t ble_ota_flash_attach(ble_ota_flash_done_fn done_cb, ble_ota_flash_abort_query_fn abort_query);

/* Creates/deletes the ble_ota_wrk worker task (FR-509/510, NFR-204).
 * Tied to fwupgrade start/stop, not to ble_ota_init(). ble_ota_flash_stop()
 * aborts any open OTA handle and is safe to call when not started. */
esp_err_t ble_ota_flash_start(void);
void      ble_ota_flash_stop(void);
bool      ble_ota_flash_is_running(void);

/* Posts an event to the worker. timeout_ticks MUST be 0 from BLE
 * callback/host-task context (FR-408, NFR-106); the console task may use
 * a small finite timeout. Returns ESP_ERR_TIMEOUT (mapped from
 * errQUEUE_FULL) if the queue is full -- callers must treat that as a
 * bounded, loud failure, never retry indefinitely. */
esp_err_t ble_ota_flash_post_event(const ble_ota_flash_evt_t *evt, TickType_t timeout_ticks);

/* The static per-part reassembly buffer L2 memcpy's chunks into. Valid
 * for the life of the component; callers must still respect
 * buffer_owner/BUF-INV (enforced by L2, not by this accessor). */
uint8_t *ble_ota_flash_get_part_buffer(void);
size_t   ble_ota_flash_get_part_buffer_size(void); /* == CONFIG_BLE_OTA_MAX_PART_SIZE */

/* FR-502/FR-907: label and size of the inactive slot esp_ota_* will
 * target for the *next* transfer (esp_ota_get_next_update_partition()).
 * out_label may be NULL if only the size is needed. Safe to call at any
 * time, including with no transfer in progress. */
esp_err_t ble_ota_flash_get_target_slot(const char **out_label, size_t *out_size);

/* FR-907/§6.6: bytes committed so far in the current (or most recently
 * finished) transfer, for the 0xEF Storage Size Report's usedBytes and
 * for "fwupgrade status". Reset to 0 by BEGIN and by FORMAT. */
uint32_t ble_ota_flash_get_used_bytes(void);

#ifdef __cplusplus
}
#endif
