/**
 * @file ble_ota_transport.h
 * @brief L1 interface: OTA client of ble_host (registration, arm/disarm, status)
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
 * NETSHELL32-5: the NimBLE host is now owned by the ble_host component, so
 * this layer no longer starts or stops BLE. It registers the OTA GATT service
 * once at boot and exposes a software arm/disarm gate (tech_spec.md 2.4).
 * No NimBLE type appears in this header's signatures; ble_ota_transport_nimble.c
 * remains the only translation unit in this component that includes a NimBLE
 * header.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ble_ota_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool            host_up;
    bool            host_starting;
    bool            registered;
    ota_arm_state_t arm_state;
    bool            adv_mode_armed;
    bool            advertising;
    bool            connected;      /* a peer is connected (shared link) */
    bool            bonded;         /* the shared link is encrypted and bonded */
    bool            cli_engaged;    /* a non-OTA client is engaged on the link */
    char            peer_addr_str[18]; /* "xx:xx:xx:xx:xx:xx", or "" if not connected */
} ble_ota_transport_status_t;

/* Registers the OTA client with ble_host and creates the notify/ctl task and queue.
 * Idempotent and attempted at most once per boot (ADV-OTA-11): later calls return the
 * stored first result and create nothing. Must run after ble_host_init() and
 * ble_ota_proto_init(), and before ble_host_start(). */
esp_err_t ble_ota_transport_register(void);
bool      ble_ota_transport_is_registered(void);
esp_err_t ble_ota_transport_registration_result(void);

typedef enum {
    OTA_ARM_OK = 0,
    OTA_ARM_ALREADY_RUNNING,
    OTA_ARM_BUSY,                 /* a start or stop is in progress, or the drain claim was lost */
    OTA_ARM_HOST_STARTING,
    OTA_ARM_HOST_UNAVAILABLE,
    OTA_ARM_NOT_REGISTERED,
    OTA_ARM_PENDING_VERIFY,
    OTA_ARM_FLASH_FAIL,           /* *flash_err holds the cause */
    OTA_ARM_HOST_TIMEOUT,
    OTA_ARM_RES_DRAIN_STUCK,      /* result name: OTA_ARM_DRAIN_STUCK is the gate STATE (ble_ota_gate.h) */
    OTA_ARM_INTERNAL,
} ble_ota_arm_result_t;

/* Arms OTA (fwupgrade start): checks in the order of tech_spec.md 2.4.6, claims
 * DISARMED -> ARMING, starts the flash worker, then arms on the host task. */
ble_ota_arm_result_t ble_ota_transport_arm(bool *peer_connected, esp_err_t *flash_err);

typedef enum {
    OTA_DISARM_OK = 0,        /* stopped, state DISARMED */
    OTA_DISARM_NOT_RUNNING,
    OTA_DISARM_BUSY,          /* a start/stop is in progress */
    OTA_DISARM_IN_PROGRESS,   /* another disarm won the race; reason in the result */
    OTA_DISARM_HOST_TIMEOUT,  /* state stays ARMED */
    OTA_DISARM_DRAIN_STUCK,
} ble_ota_disarm_kind_t;

typedef struct {
    ble_ota_disarm_kind_t kind;
    const char           *reason; /* never NULL */
} ble_ota_disarm_result_t;

/* Disarms OTA (fwupgrade stop). The link is released only if it is OTA-only
 * (REQ-FUNC-76); BLE and the CLI stay up. */
ble_ota_disarm_result_t ble_ota_transport_disarm(void);

/* Read-only: atomics and the host snapshot only; never calls ble_host_exec. */
esp_err_t ble_ota_transport_get_status(ble_ota_transport_status_t *out);

#ifdef __cplusplus
}
#endif
