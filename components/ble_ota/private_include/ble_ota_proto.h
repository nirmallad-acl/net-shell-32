/**
 * @file ble_ota_proto.h
 * @brief L2 interface: protocol state machine, called downward by L1
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
 * L2 -- no BLE-stack header, no esp_ota_* or esp_partition_* call anywhere
 * in ble_ota_proto.c (NFR-403). Includes only ble_ota_proto_defs.h (L0),
 * ble_ota_internal.h and ble_ota_flash.h (L3, downward).
 *
 * Upward data (notifications L1 must transmit) flows via a
 * downward-registered callback whose TYPE is declared here even though
 * the function body lives in L1 -- see ble_ota_proto_attach() below and
 * tech_spec.md §3.3's "upward-data problem" resolution.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Composed notification frame, ready to transmit as-is on the TX
 * characteristic. Implemented by L1; type declared here (L2). */
typedef esp_err_t (*ble_ota_notify_fn)(const uint8_t *frame, size_t len);

typedef enum {
    /* Request L1 to terminate the current BLE connection. Used only on
     * the finalize/reboot path (tech_spec.md §7.4), after the 0x0F
     * success text has been queued, so the link is torn down tidily
     * rather than merely dropped by the impending esp_restart(). */
    BLE_OTA_LINK_CTL_DISCONNECT = 0,
} ble_ota_link_ctl_cmd_t;
typedef void (*ble_ota_link_ctl_fn)(ble_ota_link_ctl_cmd_t cmd);

/* Registers L1's upward-reachable callbacks. Called once by L1 during
 * ble_ota_transport_start() (fwupgrade start bring-up). */
esp_err_t ble_ota_proto_attach(ble_ota_notify_fn notify, ble_ota_link_ctl_fn ctl);

/* One-time L2 state init: creates s_state_mtx and s_buf_released, resets
 * to state=IDLE, buffer_owner=OWNER_BLE. Called once from ble_ota_init()
 * -- NOT tied to fwupgrade start/stop, per tech_spec.md §5.9.3 (the
 * semaphore must exist "before any BLE or flash path can run"). Also
 * calls ble_ota_flash_attach() to register this layer's flash-completion
 * handler. */
esp_err_t ble_ota_proto_init(void);

/* L1 delivers one raw inbound RX-characteristic write here, running on
 * the BLE host task (tech_spec.md §3.4's task-topology table). Chunk
 * payloads are memcpy'd into the reassembly buffer inside this call --
 * they never traverse a queue (NFR-107, FR-509 permits memory work in
 * callback context; only flash work is deferred).
 *
 * link_secure is L1's own encrypted+bonded determination for the
 * connection that produced this write (FR-301's runtime check). L2 never
 * queries BLE state itself -- it only ever receives this boolean. */
void ble_ota_proto_rx(bool link_secure, const uint8_t *frame, size_t len);

/* L1 notifies L2 of connection lifecycle events so L2 can apply FR-306
 * (abort mid-transfer on encryption/bond loss) and reset per-connection
 * accounting. Safe to call with no transfer in progress. */
void ble_ota_proto_on_connect(void);
void ble_ota_proto_on_disconnect(void);

/* Idempotent abort (FR-706): a second/concurrent call while an abort is
 * already in flight is a no-op returning ESP_OK. Callable from the
 * console task (fwupgrade stop, FR-703), the BLE host task (disconnect,
 * encryption loss, protocol error) and the inactivity-timeout path. */
esp_err_t ble_ota_proto_abort(const char *reason);

/* Blocks the calling task (bounded by timeout_ms) until any pending
 * abort/drain has completed and the state machine has returned to IDLE.
 * Used by "fwupgrade stop" (tech_spec.md Phase 5 review, CRITICAL-2) so
 * the caller does not force-delete the flash worker task (L3) while the
 * ABORTING_DRAIN handoff of tech_spec.md Sec.5.9 is still in progress --
 * the worker may still own s_part_buf/the OTA handle at that instant.
 * Returns true if IDLE was reached before the timeout, false otherwise
 * (see ble_ota_proto_is_drain_stuck() for the stuck-drain case, Sec.5.9.6:
 * the caller MUST NOT force-delete the worker task in that case). Safe to
 * call when no transfer/abort is in flight (returns true immediately). */
bool ble_ota_proto_wait_idle(uint32_t timeout_ms);

/* tech_spec.md §6.7: the one-way Wi-Fi suspend hook. Registered by
 * cmd_fwupgrade.c and invoked exactly once per boot session, at the
 * PARAMS_SET -> RECEIVING transition on an accepted 0xFD (transfer-begin),
 * after s_state_mtx has been released -- the suspend action itself may
 * block on esp_wifi_* calls (NFR-106 forbids holding the mutex across
 * that). This is a plain downward hook, not a begin/end pair: r3 (§6.7.2)
 * withdrew the paired restore callback and every synchronisation
 * primitive that existed only to order it against this one (the token,
 * the semaphore, the "restore owed" flag). There is no
 * ble_ota_proto_*-side ordering requirement left to enforce -- with
 * nothing left to race, the hook is simply called and forgotten. It
 * still exists as an indirection (rather than L2 calling esp_wifi_*
 * directly) purely to preserve NFR-403's layering: L2 must never include
 * a Wi-Fi header or call an esp_wifi_* function itself. Optional: NULL is
 * a valid "nobody is listening" state and is simply never invoked. */
typedef void (*ble_ota_wifi_suspend_fn)(void);
void ble_ota_proto_set_wifi_suspend_cb(ble_ota_wifi_suspend_fn cb);

/* fwupgrade status support (FR-907). */
const char *ble_ota_proto_state_name(void);
void        ble_ota_proto_get_progress(uint32_t *bytes_received, uint32_t *bytes_declared);
bool        ble_ota_proto_is_drain_stuck(void);

#ifdef __cplusplus
}
#endif
