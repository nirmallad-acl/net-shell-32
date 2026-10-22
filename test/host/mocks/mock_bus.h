/* Host-test mock: scripted I2C bus implementing accel_bus_ops_t. A script entry
 * says what the Nth transaction returns; reads are served from a register file. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "accel_bus.h"

#define MOCK_BUS_MAX_LOG 64
#define MOCK_BUS_MAX_SCRIPT 64

typedef struct {
    char    kind;       /* 'P' probe, 'W' write, 'R' read, 'X' recover */
    uint8_t addr;
    uint8_t reg;
    uint8_t val;
    size_t  n;
} mock_bus_op_t;

typedef struct {
    uint8_t   regs[256];
    uint8_t   present_addr;               /* the address that ACKs (0 = none) */
    uint8_t   wrong_addr;                 /* another part that ACKs here with its own registers (0 = none) */
    uint8_t   wrong_regs[256];            /* register file of that other part (DEVID 0 unless set) */
    esp_err_t script[MOCK_BUS_MAX_SCRIPT]; /* result of transaction i (ESP_OK = normal); 0-length = all normal */
    size_t    script_len;
    esp_err_t fail_all;                   /* if non-zero every transaction returns this */
    uint32_t  latency_ms_on_timeout;      /* mock time added when a transaction times out */
    mock_bus_op_t log[MOCK_BUS_MAX_LOG];
    size_t    n_ops;                      /* total transactions issued */
    size_t    n_writes;
    size_t    n_recovers;
    int64_t   now_us;                     /* the mock clock, advanced by timeouts */
} mock_bus_t;

extern const accel_bus_ops_t mock_bus_ops;
void mock_bus_reset(mock_bus_t *b);
