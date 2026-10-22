/**
 * @file accel_bus_i2c.c
 * @brief ESP-IDF i2c_master glue implementing accel_bus_ops_t
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
 * REQ-HW-03..05, REQ-FUNC-52, REQ-BLD-01 (non-legacy i2c_master API only).
 * Every call passes a finite xfer timeout, never -1.
 */
#include "accel_bus_i2c.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "adxl345_regs.h"

static const char *TAG = "accel";

/* Compile-time pin sanity (REQ-HW-02/03): never UART0 (1, 3), SPI flash (6..11),
 * and SDA/SCL need output drivers so the input-only GPIOs 34..39 are excluded. */
/* Also the strapping pins 0/2/5/12/15 (REQ-HW-03): a sensor board pulling GPIO12 high at
 * reset selects 1.8 V flash and the board stops booting, GPIO0 low enters download mode.
 * This guard is a C _Static_assert, not a CMake check; Kconfig cannot exclude values from
 * a range, so a bad menuconfig value fails the build here. */
#define PIN_FORBIDDEN(p) ((p) < 0 || (p) == 0 || (p) == 1 || (p) == 2 || (p) == 3 || (p) == 5 || \
                          ((p) >= 6 && (p) <= 11) || (p) == 12 || (p) == 15 || \
                          ((p) >= 34 && (p) <= 39) || (p) > 33)
_Static_assert(!PIN_FORBIDDEN(CONFIG_ACCEL_I2C_SDA_GPIO), "ACCEL_I2C_SDA_GPIO is a forbidden/input-only pin");
_Static_assert(!PIN_FORBIDDEN(CONFIG_ACCEL_I2C_SCL_GPIO), "ACCEL_I2C_SCL_GPIO is a forbidden/input-only pin");
_Static_assert(CONFIG_ACCEL_I2C_SDA_GPIO != CONFIG_ACCEL_I2C_SCL_GPIO, "SDA and SCL must differ");

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev[2];
    uint8_t                 addr[2];
} bus_ctx_t;

static bus_ctx_t s_ctx;
static bool      s_created;

static esp_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:
        return ESP_OK;
    case ESP_ERR_NOT_FOUND:         /* i2c_master_probe NACK */
    case ESP_ERR_INVALID_RESPONSE:  /* transaction NACK */
        return ESP_ERR_ACCEL_NOT_PRESENT;
    case ESP_ERR_TIMEOUT:
        return ESP_ERR_ACCEL_TIMEOUT;
    default:
        return ESP_ERR_ACCEL_BUS;
    }
}

static i2c_master_dev_handle_t dev_for(bus_ctx_t *c, uint8_t addr)
{
    for (int i = 0; i < 2; i++) {
        if (c->addr[i] == addr) {
            return c->dev[i];
        }
    }
    return NULL;
}

static esp_err_t bus_probe(void *bus, uint8_t addr, uint32_t timeout_ms)
{
    bus_ctx_t *c = (bus_ctx_t *)bus;
    if (c == NULL || c->bus == NULL) {
        return ESP_ERR_ACCEL_BUS;
    }
    return map_err(i2c_master_probe(c->bus, addr, (int)timeout_ms));
}

static esp_err_t bus_write_reg(void *bus, uint8_t addr, uint8_t reg, uint8_t val, uint32_t timeout_ms)
{
    bus_ctx_t *c = (bus_ctx_t *)bus;
    i2c_master_dev_handle_t dev = (c != NULL) ? dev_for(c, addr) : NULL;
    if (dev == NULL) {
        return ESP_ERR_ACCEL_BUS;
    }
    const uint8_t frame[2] = { reg, val };
    return map_err(i2c_master_transmit(dev, frame, sizeof(frame), (int)timeout_ms));
}

static esp_err_t bus_read_regs(void *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n,
                               uint32_t timeout_ms)
{
    bus_ctx_t *c = (bus_ctx_t *)bus;
    i2c_master_dev_handle_t dev = (c != NULL) ? dev_for(c, addr) : NULL;
    if (dev == NULL || buf == NULL || n == 0) {
        return ESP_ERR_ACCEL_BUS;
    }
    /* One transmit_receive: repeated start, so X/Y/Z come from one conversion. */
    return map_err(i2c_master_transmit_receive(dev, &reg, 1, buf, n, (int)timeout_ms));
}

static esp_err_t bus_recover(void *bus)
{
    bus_ctx_t *c = (bus_ctx_t *)bus;
    if (c == NULL || c->bus == NULL) {
        return ESP_ERR_ACCEL_BUS;
    }
    return map_err(i2c_master_bus_reset(c->bus));
}

static const accel_bus_ops_t s_ops = {
    .probe = bus_probe,
    .write_reg = bus_write_reg,
    .read_regs = bus_read_regs,
    .recover = bus_recover,
};

void accel_bus_i2c_destroy(void)
{
    if (!s_created) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (s_ctx.dev[i] != NULL) {
            (void)i2c_master_bus_rm_device(s_ctx.dev[i]);
            s_ctx.dev[i] = NULL;
        }
    }
    if (s_ctx.bus != NULL) {
        (void)i2c_del_master_bus(s_ctx.bus);
        s_ctx.bus = NULL;
    }
    s_created = false;
}

esp_err_t accel_bus_i2c_create(const accel_bus_ops_t **ops, void **bus)
{
    if (ops == NULL || bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_created) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CONFIG_ACCEL_I2C_PORT,
        .sda_io_num = CONFIG_ACCEL_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_ACCEL_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* REQ-HW-05: internal pull-ups on */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_ctx.bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    static const uint8_t addrs[2] = { ADXL345_ADDR_PRIMARY, ADXL345_ADDR_ALTERNATE };
    for (int i = 0; i < 2; i++) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = CONFIG_ACCEL_I2C_FREQ_HZ,
        };
        err = i2c_master_bus_add_device(s_ctx.bus, &dev_cfg, &s_ctx.dev[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02x) failed: %s", addrs[i], esp_err_to_name(err));
            /* Clean exit: release everything created so far. */
            for (int j = 0; j < i; j++) {
                (void)i2c_master_bus_rm_device(s_ctx.dev[j]);
                s_ctx.dev[j] = NULL;
            }
            (void)i2c_del_master_bus(s_ctx.bus);
            s_ctx.bus = NULL;
            return err;
        }
        s_ctx.addr[i] = addrs[i];
    }
    s_created = true;
    *ops = &s_ops;
    *bus = &s_ctx;
    return ESP_OK;
}
