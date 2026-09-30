/**
 * @file ble_ota_proto_defs.c
 * @brief L0 protocol constant tables: definitions
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
 * Defined once here (rather than as "static const" in the header) so
 * every including translation unit shares one copy and none of them can
 * warn about an unused table it does not happen to reference.
 */
#include "ble_ota_proto_defs.h"

#include <stddef.h>

const ble_ota_rx_frame_desc_t ble_ota_rx_opcodes[] = {
    { BLE_OTA_OP_SET_FILE_SIZE,   5, 0, "SET_FILE_SIZE"   },
    { BLE_OTA_OP_SET_XFER_PARAMS, 5, 0, "SET_XFER_PARAMS" },
    { BLE_OTA_OP_START_TRANSFER,  1, 0, "START_TRANSFER"  },
    { BLE_OTA_OP_DATA_CHUNK,      0, 3, "DATA_CHUNK"      }, /* 2 + N, N >= 1 */
    { BLE_OTA_OP_END_OF_PART,     5, 0, "END_OF_PART"     },
    { BLE_OTA_OP_FORMAT_STORAGE,  1, 0, "FORMAT_STORAGE"  },
};
const size_t ble_ota_rx_opcodes_count =
    sizeof(ble_ota_rx_opcodes) / sizeof(ble_ota_rx_opcodes[0]);

const ble_ota_tx_frame_desc_t ble_ota_tx_notifications[] = {
    { BLE_OTA_NOTIFY_MODE_ACK,      2, "MODE_ACK"      },
    { BLE_OTA_NOTIFY_STORAGE_SIZE,  7, "STORAGE_SIZE"  },
    { BLE_OTA_NOTIFY_REQ_NEXT_PART, 3, "REQ_NEXT_PART" },
    { BLE_OTA_NOTIFY_XFER_COMPLETE, 3, "XFER_COMPLETE" },
    { BLE_OTA_NOTIFY_OTA_RESULT,    0, "OTA_RESULT"    }, /* variable */
};
const size_t ble_ota_tx_notifications_count =
    sizeof(ble_ota_tx_notifications) / sizeof(ble_ota_tx_notifications[0]);

const ble_ota_rx_frame_desc_t *ble_ota_rx_frame_desc_find(uint8_t opcode)
{
    for (size_t i = 0; i < ble_ota_rx_opcodes_count; i++) {
        if (ble_ota_rx_opcodes[i].opcode == opcode) {
            return &ble_ota_rx_opcodes[i];
        }
    }
    return NULL;
}
