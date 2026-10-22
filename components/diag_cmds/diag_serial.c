/**
 * @file diag_serial.c
 * @brief Serial transport: esp_console adapters, serial sink, hw_rev_set
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
 * REQ-FUNC-12, 14, 16, 33; tech_spec.md 2.6.1 and 2.6.2. Each command is
 * registered with func_w_context on the EXISTING console (no
 * esp_console_new_repl_* call here). Adapters always return 0 after emitting
 * a line, so the REPL does not append its own error text and the serial
 * output stays identical to BLE.
 */
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "device_info.h"
#include "diag_format.h"
#include "diag_internal.h"

static const char *TAG = DIAG_TAG;

#ifdef CONFIG_ESP_CONSOLE_UART_NUM
#define DIAG_CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#else
#define DIAG_CONSOLE_UART 0
#endif

/* ---- serial sink (tech_spec.md 2.6.1) ---- */

/* One fwrite() of the whole terminated line: newlib/picolibc holds the stdout
 * FILE lock for one fwrite, so an ESP_LOG line from another task can appear
 * only between two response lines, never inside one. The VFS converts LF to
 * CRLF (the existing console convention). deadline_us is ignored: one write of
 * at most 128 B is about 11 ms at 115200 baud. */
static esp_err_t serial_write_line(const diag_ctx_t *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (line == NULL || len == 0 || len > DIAG_FMT_LINE_MAX_CHARS) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[DIAG_LINE_MAX + 1];
    memcpy(buf, line, len);
    buf[len] = '\n';
    size_t w = fwrite(buf, 1, len + 1u, stdout);
    (void)fflush(stdout);
    return (w == len + 1u) ? ESP_OK : ESP_FAIL;
}

/* uart_wait_tx_done() is finite, unlike fsync() (REQ-REL-04). */
static esp_err_t serial_flush(const diag_ctx_t *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    (void)fflush(stdout);
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    return uart_wait_tx_done((uart_port_t)DIAG_CONSOLE_UART, t);
}

static bool serial_is_current(const diag_ctx_t *ctx)
{
    (void)ctx;
    return true;
}

static diag_ctx_t s_serial_ctx = {
    .transport = DIAG_TRANSPORT_SERIAL,
    .gen = 0,
    .deadline_us = 0,
    .write_line = serial_write_line,
    .flush = serial_flush,
    .is_current = serial_is_current,
};

/* ---- console adapters ---- */

typedef struct {
    const char   *name;
    const char   *help;
    diag_cmd_id_t id;
} serial_reg_t;

static serial_reg_t s_regs[DIAG_CMD_COUNT] = {
    { "hw_version",    "Print the hardware revision (set once with hw_rev_set)", DIAG_CMD_HW_VERSION },
    { "fw_version",    "Print the firmware version of the running image", DIAG_CMD_FW_VERSION },
    { "accel_xyz",     "Stream 15 acceleration samples at 1 s intervals", DIAG_CMD_ACCEL_XYZ },
    { "reboot_reason", "Print why the device last reset", DIAG_CMD_REBOOT_REASON },
    { "reboot",        "Restart the device (refused during OTA or PENDING_VERIFY)", DIAG_CMD_REBOOT },
};

static int serial_adapter(void *context, int argc, char **argv)
{
    (void)argv;
    const serial_reg_t *r = (const serial_reg_t *)context;
    if (r == NULL) {
        return 0;
    }
    if (argc != 1) {
        (void)diag_emit_error(&s_serial_ctx, "invalid_argument", r->name, NULL); /* REQ-FUNC-16 */
        return 0;
    }
    s_serial_ctx.t_rx_ms = diag_now_ms(); /* single REPL task: no race on this field */
    (void)diag_cmd_run(r->id, &s_serial_ctx);
    return 0;
}

/* hw_rev_set <rev>: serial only, not in the BLE table (REQ-FUNC-33). */
static int hw_rev_set_adapter(void *context, int argc, char **argv)
{
    (void)context;
    if (argc != 2 || argv == NULL || argv[1] == NULL || !device_info_hw_revision_is_valid(argv[1])) {
        (void)diag_emit_error(&s_serial_ctx, "invalid_argument", "hw_rev_set", NULL);
        return 0;
    }
    esp_err_t err = device_info_set_hw_revision(argv[1]);
    if (err != ESP_OK) {
        (void)diag_emit_error(&s_serial_ctx, "internal", "hw_rev_set", NULL);
        return 0;
    }
    char line[DIAG_LINE_MAX];
    if (diag_fmt_ok_kv(line, sizeof(line), "hw_rev_set", argv[1]) == ESP_OK) {
        (void)diag_write_line(&s_serial_ctx, line);
    }
    return 0;
}

esp_err_t diag_serial_register(void)
{
    esp_err_t first_err = ESP_OK;
    for (int i = 0; i < (int)DIAG_CMD_COUNT; i++) {
        const esp_console_cmd_t cmd = {
            .command = s_regs[i].name,
            .help = s_regs[i].help,
            .hint = NULL,
            .func = NULL,
            .argtable = NULL,
            .func_w_context = serial_adapter,
            .context = &s_regs[i],
        };
        esp_err_t err = esp_console_cmd_register(&cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "console registration of '%s' failed (%s)", s_regs[i].name, esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }

    const esp_console_cmd_t rev_cmd = {
        .command = "hw_rev_set",
        .help = "Store the hardware revision once at provisioning: hw_rev_set <rev> ([A-Za-z0-9._-], 1-32 chars)",
        .hint = NULL,
        .func = NULL,
        .argtable = NULL,
        .func_w_context = hw_rev_set_adapter,
        .context = NULL,
    };
    esp_err_t err = esp_console_cmd_register(&rev_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console registration of 'hw_rev_set' failed (%s)", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}
