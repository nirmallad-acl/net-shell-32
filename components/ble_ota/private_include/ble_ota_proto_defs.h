/**
 * @file ble_ota_proto_defs.h
 * @brief L0 protocol constants: GATT UUIDs, RX opcode and TX notification tables
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
 * Single well-identified source of truth for every protocol constant
 * (NFR-407), transcribed verbatim from BLE_OTA_Wire_Protocol_Spec.md.
 * No opcode, UUID or field beyond that spec is invented here.
 *
 * This is an L0 leaf: it MUST NOT include any BLE-stack header (NimBLE or
 * Bluedroid) so that L2 (which must not include BLE headers, NFR-403) can
 * include it too. UUIDs are therefore expressed as raw byte-list macros,
 * not as a BLE-library UUID type; the transport layer (L1) assembles its
 * own library-specific UUID objects from these bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * GATT service/characteristic UUIDs
 * (BLE_OTA_Wire_Protocol_Spec.md, UUID table)
 *
 * Byte order below is little-endian -- i.e. the reverse of the UUID's
 * canonical big-endian string form -- matching NimBLE's ble_uuid128_t
 * value[16] convention, which is the only consumer of these macros.
 * ------------------------------------------------------------------- */

/* fb1e4001-54ae-4a28-9f74-dfccb248601d */
#define BLE_OTA_SVC_UUID128_BYTES_LE \
    0x1D, 0x60, 0x48, 0xB2, 0xCC, 0xDF, 0x74, 0x9F, \
    0x28, 0x4A, 0xAE, 0x54, 0x01, 0x40, 0x1E, 0xFB

/* fb1e4002-54ae-4a28-9f74-dfccb248601d -- RX (App -> Device), Write + Write-Without-Response */
#define BLE_OTA_RX_CHR_UUID128_BYTES_LE \
    0x1D, 0x60, 0x48, 0xB2, 0xCC, 0xDF, 0x74, 0x9F, \
    0x28, 0x4A, 0xAE, 0x54, 0x02, 0x40, 0x1E, 0xFB

/* fb1e4003-54ae-4a28-9f74-dfccb248601d -- TX (Device -> App), Notify + CCCD */
#define BLE_OTA_TX_CHR_UUID128_BYTES_LE \
    0x1D, 0x60, 0x48, 0xB2, 0xCC, 0xDF, 0x74, 0x9F, \
    0x28, 0x4A, 0xAE, 0x54, 0x03, 0x40, 0x1E, 0xFB

/* Advertised device name (OQ-2 residual decision, tech_spec.md §11 OQ-2). */
#define BLE_OTA_ADV_DEVICE_NAME "NETSHELL32"

/* ---------------------------------------------------------------------
 * RX opcodes: App -> Device, written to the RX characteristic
 * (BLE_OTA_Wire_Protocol_Spec.md §2)
 * ------------------------------------------------------------------- */
#define BLE_OTA_OP_SET_FILE_SIZE   0xFEu /* 5 B: 32-bit BE size */
#define BLE_OTA_OP_SET_XFER_PARAMS 0xFFu /* 5 B: 16-bit BE parts + 16-bit BE chunkSize */
#define BLE_OTA_OP_START_TRANSFER  0xFDu /* 1 B, no payload */
#define BLE_OTA_OP_DATA_CHUNK      0xFBu /* 2+N B: chunkIndex (1B) + payload */
#define BLE_OTA_OP_END_OF_PART     0xFCu /* 5 B: 16-bit BE partLen + 16-bit BE partIndex */
#define BLE_OTA_OP_FORMAT_STORAGE  0xEFu /* 1 B, no payload -- erase/reformat */

/* ---------------------------------------------------------------------
 * TX notification opcodes: Device -> App, via the TX characteristic
 * (BLE_OTA_Wire_Protocol_Spec.md §3)
 *
 * FR-409: kept in a table SEPARATE from the RX opcodes above. 0xEF is a
 * deliberate, coincidental, direction-keyed reuse (Format Storage command
 * vs. Storage Size Report notification) -- the two must never share one
 * dispatch table or enum.
 * ------------------------------------------------------------------- */
#define BLE_OTA_NOTIFY_MODE_ACK      0xAAu /* 2 B: mode (0=slow,1=fast) */
#define BLE_OTA_NOTIFY_STORAGE_SIZE  0xEFu /* 7 B: 24-bit BE totalBytes + 24-bit BE usedBytes */
#define BLE_OTA_NOTIFY_REQ_NEXT_PART 0xF1u /* 3 B: 16-bit BE nextPartIndex */
#define BLE_OTA_NOTIFY_XFER_COMPLETE 0xF2u /* 3 B: 16-bit BE partIndex */
#define BLE_OTA_NOTIFY_OTA_RESULT    0x0Fu /* 1+N B: free-form ASCII */

/* Largest notification frame this component ever composes. Bounds
 * s_notify_q's item size and the 0x0F payload so it fits an
 * un-negotiated MTU-23 notification (tech_spec.md §3.4, §6.6). */
#define BLE_OTA_NOTIFY_MAX_LEN 20u

/* Ceiling on a declared 0xFE image size -- the OTA app-slot size from
 * partitions.csv (tech_spec.md §4.1). A larger declared size is rejected
 * at declaration time (FR-406), before any erase or write. */
#define BLE_OTA_MAX_IMAGE_SIZE 0x1F0000u

/* ---------------------------------------------------------------------
 * Frame-length validation tables (FR-405, §6.3): checked before any
 * field of an inbound/outbound frame is read.
 * ------------------------------------------------------------------- */

/* fixed_len == 0 means "variable length" (0xFB only): the frame must be
 * >= min_len, and its payload is separately bounded against the
 * negotiated chunkSize/MAX_PART_SIZE by the protocol layer (§5.3). */
typedef struct {
    uint8_t     opcode;
    size_t      fixed_len;
    size_t      min_len;
    const char *name;
} ble_ota_rx_frame_desc_t;

extern const ble_ota_rx_frame_desc_t ble_ota_rx_opcodes[];
extern const size_t                  ble_ota_rx_opcodes_count;

/* Looks up a RX opcode's frame-length rule. Returns NULL if opcode is not
 * one of the six RX opcodes defined above. */
const ble_ota_rx_frame_desc_t *ble_ota_rx_frame_desc_find(uint8_t opcode);

/* fixed_len == 0 means "variable length" (0x0F only). */
typedef struct {
    uint8_t     opcode;
    size_t      fixed_len;
    const char *name;
} ble_ota_tx_frame_desc_t;

extern const ble_ota_tx_frame_desc_t ble_ota_tx_notifications[];
extern const size_t                  ble_ota_tx_notifications_count;

#ifdef __cplusplus
}
#endif
