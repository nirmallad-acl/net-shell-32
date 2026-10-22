/**
 * @file ble_host_client.h
 * @brief Client-facing interface of ble_host (registration, exec, notify, link info)
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
 */

/*
 * Used by ble_ota and ble_cli. The single deviation from the "only
 * <stdbool.h>/esp_err.h types" public-header convention is the forward
 * declaration of struct ble_gatt_svc_def below (accepted at Gate 2, G2-03):
 * no NimBLE header is included and no NimBLE type is defined here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ble_gatt_svc_def; /* forward declaration only (G2-03) */

typedef uint8_t ble_host_client_id_t;

typedef enum {
    BLE_HOST_ADV_ALWAYS = 0,
    BLE_HOST_ADV_WHEN_PRIMARY,
} ble_host_adv_policy_t;

typedef enum {
    BLE_HOST_EVT_CONNECT = 0,
    BLE_HOST_EVT_DISCONNECT,
    BLE_HOST_EVT_ENC_CHANGE,
    BLE_HOST_EVT_SUBSCRIBE,
    BLE_HOST_EVT_MTU,
    BLE_HOST_EVT_REPEAT_PAIRING,
    BLE_HOST_EVT_HOST_RESET,
    BLE_HOST_EVT_SYNC, /* first sync of the boot only (additive; GATT handles are assigned) */
} ble_host_evt_type_t;

/* SUBSCRIBE sub_reason values. */
#define BLE_HOST_SUB_WRITE   1u /* peer wrote the CCCD */
#define BLE_HOST_SUB_RESTORE 2u /* bonded CCCD restored after encryption */
#define BLE_HOST_SUB_TERM    3u /* link ended */

typedef struct {
    ble_host_evt_type_t type;
    uint16_t conn_handle;
    uint32_t gen;         /* connection generation after this event was applied */
    bool     secure;      /* encrypted && bonded, snapshot at dispatch */
    int      status;      /* ENC_CHANGE status / DISCONNECT reason / 0 */
    uint16_t attr_handle; /* SUBSCRIBE: characteristic VALUE handle; each client compares it
                             with its own TX val_handle and ignores every other handle */
    bool     notify;      /* SUBSCRIBE: cur_notify */
    bool     prev_notify; /* SUBSCRIBE: prev_notify */
    uint8_t  sub_reason;  /* SUBSCRIBE: BLE_HOST_SUB_* */
    uint16_t mtu;         /* MTU: negotiated value */
    char     peer_addr[18]; /* "xx:xx:xx:xx:xx:xx" (identity address) */
} ble_host_evt_t;

typedef void (*ble_host_evt_cb_t)(const ble_host_evt_t *evt, void *user);

/* Exec functions take their argument BY VALUE and return their outcome BY
 * VALUE. They must never dereference memory owned by the caller (the caller
 * may have returned on ESP_ERR_BLE_HOST_EXEC_INDETERMINATE while fn is still
 * running). Shared state is reached only through the owning module's
 * file-scope statics/atomics. */
typedef int32_t (*ble_host_exec_fn_t)(uintptr_t arg);

typedef struct {
    const char                    *name;            /* "ota", "cli" (logs only) */
    const struct ble_gatt_svc_def *svcs;            /* NULL-terminated, static lifetime */
    const uint8_t                 *adv_uuid128;     /* 16 bytes, little-endian, static lifetime */
    ble_host_adv_policy_t          adv_policy;
    bool                           default_primary; /* exactly one client sets this (the CLI) */
    ble_host_evt_cb_t              cb;              /* runs on the host task, must return in <= 5 ms */
    void                          *user;
    bool                           waive_timing;    /* callback time excluded from cb_max_us
                                                       (OTA: REQ-PERF-03 waiver, tech_spec 2.4.9) */
} ble_host_client_cfg_t;

/* Before ble_host_start() only. ESP_ERR_INVALID_STATE after start,
 * ESP_ERR_INVALID_ARG on NULL/duplicate name or svcs, ESP_ERR_NO_MEM over
 * CONFIG_BLE_HOST_MAX_CLIENTS. Changes nothing on error. */
esp_err_t ble_host_register_client(const ble_host_client_cfg_t *cfg, ble_host_client_id_t *out_id);

#define ESP_ERR_BLE_HOST_BASE               0xB100
#define ESP_ERR_BLE_HOST_NOT_UP             (ESP_ERR_BLE_HOST_BASE + 1) /* INIT, STARTING, FAILED or re-syncing */
#define ESP_ERR_BLE_HOST_WRONG_TASK         (ESP_ERR_BLE_HOST_BASE + 2) /* called from the host task */
#define ESP_ERR_BLE_HOST_EXEC_INDETERMINATE (ESP_ERR_BLE_HOST_BASE + 3) /* fn started but did not finish in time */

/* Run fn(arg) on the host task, serialised with all GAP/GATT callbacks.
 * Blocks the caller for at most timeout_ms IN TOTAL (one deadline covering
 * the exec mutex take, the wait for fn to start and the completion window);
 * timeout_ms must be > 350 ms, else ESP_ERR_INVALID_ARG.
 * ESP_OK = fn completed and *out_result (if non-NULL) holds fn's return value;
 * ESP_OK says NOTHING about whether fn changed any state.
 * ESP_ERR_TIMEOUT = fn never ran and never will.
 * ESP_ERR_BLE_HOST_EXEC_INDETERMINATE = fn started and had not finished;
 * *out_result is not written. ESP_ERR_BLE_HOST_NOT_UP / _WRONG_TASK: fn never ran.
 * Every exec fn must be non-blocking except for at most two takes of the
 * ble_ota_proto mutex, each bounded at 100 ms. */
esp_err_t ble_host_exec(ble_host_exec_fn_t fn, uintptr_t arg, uint32_t timeout_ms, int32_t *out_result);

/* Host-task-only helpers (called from client callbacks or exec functions). */
esp_err_t ble_host_set_adv_primary(ble_host_client_id_t id, bool primary); /* re-applies payload */
void      ble_host_note_engaged(ble_host_client_id_t id, uint32_t gen);
esp_err_t ble_host_release_link(ble_host_client_id_t id, uint32_t gen);   /* REQ-FUNC-76 policy */

/* Any-task helpers. */
uint32_t  ble_host_current_gen(void);
bool      ble_host_link_secure(uint16_t conn_handle);    /* live: encrypted && bonded */
uint16_t  ble_host_att_mtu(uint16_t conn_handle);        /* 23 if unknown */
/* Sends one notification. expected_gen is the connection generation the data belongs to:
 * ESP_ERR_INVALID_STATE (nothing sent) unless the link is up, conn_handle is it, its
 * generation equals expected_gen and the link is encrypted and bonded right now.
 * ESP_ERR_NO_MEM on mbuf/congestion (transient, may be retried). */
esp_err_t ble_host_notify(uint16_t conn_handle, uint16_t val_handle, const uint8_t *data, size_t len,
                          uint32_t expected_gen);
esp_err_t ble_host_terminate(uint16_t conn_handle);
/* ESP_OK if a link exists (any output pointer may be NULL), else ESP_ERR_INVALID_STATE. */
esp_err_t ble_host_get_link(uint16_t *conn_handle, uint32_t *gen, bool *secure, char peer_addr[18]);
bool      ble_host_is_advertising(void);
uint32_t  ble_host_engaged_mask(void);                   /* bit i set: client i engaged on the current link */

/* Instrumentation (REQ-PERF-03, V-05): maximum dispatched callback time
 * excluding waived clients, and the maximum OTA RX callback time reported
 * by ble_ota through ble_host_note_ota_rx_us(). */
uint32_t  ble_host_cb_max_us(void);
uint32_t  ble_host_ota_rx_max_us(void);
void      ble_host_note_ota_rx_us(uint32_t us);

#ifdef __cplusplus
}
#endif
