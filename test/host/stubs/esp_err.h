/* Host-test stub of esp_err.h: only the codes the PURE sources use (values
 * copied from ESP-IDF v6.0.2 esp_err.h). Not part of the firmware build. */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT       0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
