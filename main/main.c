/**
 * @file main.c
 * @brief Application entry point: boot sequence orchestration and REPL initialization
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-09-07
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
 * Boot sequence: NVS, netif, event loop, Wi-Fi, CLI, REPL.
 *
 * IMPORTANT: The boot sequence is CONDITIONAL, not a flat series of calls.
 * Some steps (e.g., esp_netif_create_default_wifi_sta()) abort internally
 * if their prerequisites failed, so each step only runs if its dependencies
 * have already succeeded. Steps that are skipped record NET_CLI_ERR_SKIPPED,
 * not a success code.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "accel_sensor.h"
#include "ble_cli.h"
#include "ble_host.h"
#include "ble_ota.h"
#include "device_info.h"
#include "diag_cmds.h"
#include "net_cli.h"

static const char *TAG = "main";

#if CONFIG_NS5_TEST_AUTORUN
/* BUILD-TEST ONLY (hook H-11 fallback): runs a scripted command list through the
 * console after boot completed. Never part of a shipped image. */
static void ns5_autorun_task(void *arg)
{
    (void)arg;
    static char cmds[] = CONFIG_NS5_TEST_AUTORUN_CMDS;
    vTaskDelay(pdMS_TO_TICKS(5000));
    char *save = NULL;
    for (char *tok = strtok_r(cmds, ";", &save); tok != NULL; tok = strtok_r(NULL, ";", &save)) {
        int ret = 0;
        esp_err_t e = esp_console_run(tok, &ret);
        ESP_LOGI(TAG, "NS5_AUTORUN cmd='%s' err=%s ret=%d", tok, esp_err_to_name(e), ret);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}
#endif

#if CONFIG_NS5_TEST_STALL_HOST
/* Defined in ble_host.c under the same option; deliberately NOT in a public header. */
esp_err_t ble_host_test_stall(uint32_t ms);

/* BUILD-TEST ONLY (hook H-10): ns5_stall <ms> blocks the NimBLE host task. */
static int cmd_ns5_stall(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: ns5_stall <ms>\n");
        return 1;
    }
    long ms = strtol(argv[1], NULL, 10);
    if (ms <= 0 || ms > 30000) {
        printf("ns5_stall: ms must be 1..30000\n");
        return 1;
    }
    return (ble_host_test_stall((uint32_t)ms) == ESP_OK) ? 0 : 1;
}
#endif

/* REPL: held for the program lifetime, never torn down. */
static esp_console_repl_t *s_repl = NULL;

/* Standard NVS erase-and-retry: if init fails with "no free pages" or
 * "new version", erase and try once more. Checked rather than assumed
 * to avoid aborting on a diagnostic error. */
static bool s_nvs_erased; /* the erase-and-retry path ran: hw revision and BLE bonds are gone */

static esp_err_t boot_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s); erasing and retrying", esp_err_to_name(err));
        s_nvs_erased = true;
        esp_err_t erase_err = nvs_flash_erase();
        err = (erase_err == ESP_OK) ? nvs_flash_init() : erase_err;
    }
    return err;
}

void app_main(void)
{
    net_cli_boot_status_t st = { 0 };

    /* Boot step 1: NVS */
    st.nvs_init = boot_nvs();

    /* Boot step 2: Network interface */
    st.netif_init = esp_netif_init();

    /* Boot step 3: Event loop */
    st.event_loop = esp_event_loop_create_default();

    bool netif_ok = (st.netif_init == ESP_OK);
    bool loop_ok  = (st.event_loop == ESP_OK);

    /* Boot step 4: Wi-Fi STA interface (depends on netif + event loop).
     * esp_netif_create_default_wifi_sta() aborts internally if prerequisites
     * failed, so it must not be called unless both netif and loop succeeded. */
    if (netif_ok && loop_ok) {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        st.netif_create_sta = (sta != NULL) ? ESP_OK : ESP_FAIL;
    } else {
        st.netif_create_sta = NET_CLI_ERR_SKIPPED;
    }

    /* Boot step 5: Wi-Fi init (depends on netif_create_sta) */
    if (st.netif_create_sta == ESP_OK) {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        st.wifi_init = esp_wifi_init(&wifi_cfg);
    } else {
        st.wifi_init = NET_CLI_ERR_SKIPPED;
    }

    /* Boot step 6: Wi-Fi mode (depends on wifi_init) */
    st.wifi_set_mode = (st.wifi_init == ESP_OK) ? esp_wifi_set_mode(WIFI_MODE_STA)
                                                 : NET_CLI_ERR_SKIPPED;

    /* Boot step 7: CLI state init (depends on event loop only).
     * Worth creating even if Wi-Fi itself fails. Handler registration here,
     * before wifi_start, ensures no association events are missed. */
    st.cli_init = loop_ok ? net_cli_init() : NET_CLI_ERR_SKIPPED;

    /* Boot step 8: Start Wi-Fi (depends on wifi_set_mode) */
    st.wifi_start = (st.wifi_set_mode == ESP_OK) ? esp_wifi_start()
                                                  : NET_CLI_ERR_SKIPPED;

    /* Boot step 9: Publish boot status (by-value copy). */
    net_cli_set_boot_status(&st);

    /* Boot step 9.4 (NETSHELL32-5): shared BLE host component init. Zeroes the
     * client table and creates static sync objects; no radio. Must precede every
     * client registration. On failure later host steps are skipped
     * (ble_host logs BLECLI_HOST_FAIL itself). */
    esp_err_t host_init_err = ble_host_init();

    /* Boot step 9.5: BLE OTA component init (NETSHELL32-2, FR-101/FR-809).
     * Never touches the BLE controller here; only creates this
     * component's own sync primitives, registers the OTA GATT service with
     * ble_host and evaluates the post-boot rollback/health state. A failure
     * is logged and is NOT fatal to the rest of boot (degraded-boot
     * philosophy, consistent with the other steps below).
     *
     * INVARIANT I-1 (tech_spec.md 2.3.1): the OTA client MUST be registered
     * BEFORE the CLI client (step 9.8). NimBLE assigns handles in
     * registration order, so the OTA service/RX/TX handles stay identical to
     * NETSHELL32-2 and only the CLI handles are new. Do not reorder 9.5 and
     * 9.8. */
    esp_err_t ble_ota_init_err = ble_ota_init();
    if (ble_ota_init_err != ESP_OK) {
        ESP_LOGE(TAG, "ble_ota_init failed (%s)", esp_err_to_name(ble_ota_init_err));
    }

    /* Boot step 9.6 (NETSHELL32-5): firmware/hardware revision and reset reason. */
    esp_err_t devinfo_err = device_info_init(st.nvs_init == ESP_OK);
    if (devinfo_err != ESP_OK) {
        ESP_LOGW(TAG, "device_info_init failed (%s)", esp_err_to_name(devinfo_err));
    }
    if (s_nvs_erased) {
        /* One distinct warning (ADV-IMPL-11): the erase also wiped the stored hardware
         * revision and every BLE bond. Not an error level: the board still works. */
        ESP_LOGW(TAG, "NVS was erased at boot: hw_version will report 'unavailable' until you "
                      "re-provision it with 'hw_rev_set <rev>' over serial, and any phone must "
                      "forget the device before pairing again");
    }

    /* Boot step 9.7: accelerometer (I2C bus, probe task). A failure leaves the
     * simulator path fully working (REQ-REL-01). */
    esp_err_t accel_err = accel_sensor_init();
    if (accel_err != ESP_OK) {
        ESP_LOGW(TAG, "accel_sensor_init failed (%s); simulator-only", esp_err_to_name(accel_err));
    }

    /* Boot step 9.8: shared command layer, then the BLE CLI client (registered
     * AFTER ble_ota, see invariant I-1 above). */
    esp_err_t diag_err = diag_cmds_init();
    if (diag_err != ESP_OK) {
        ESP_LOGW(TAG, "diag_cmds_init failed (%s)", esp_err_to_name(diag_err));
    }
    if (host_init_err == ESP_OK) {
        esp_err_t cli_err = ble_cli_init();
        if (cli_err != ESP_OK) {
            ESP_LOGE(TAG, "ble_cli_init failed (%s)", esp_err_to_name(cli_err));
        }
    }

    /* Boot step 10: Set log levels (before REPL to avoid noisy output). */
    esp_log_level_set("wifi",               ESP_LOG_WARN);
    esp_log_level_set("wifi_init",          ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip",     ESP_LOG_WARN);
    esp_log_level_set("phy_init",           ESP_LOG_WARN);

    /* Boot step 11: Configure REPL over UART0 (115200 8N1). */
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uart_cfg.channel     = 0;
    uart_cfg.baud_rate   = 115200;
    uart_cfg.tx_gpio_num = -1;        /* Board default pins. */
    uart_cfg.rx_gpio_num = -1;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt             = "esp32_net>";   /* No trailing space. */
    repl_cfg.max_history_len    = 16;
    repl_cfg.history_save_path  = NULL;
    repl_cfg.task_stack_size    = 8192;
    repl_cfg.task_priority      = 2;
    repl_cfg.task_core_id       = tskNO_AFFINITY;
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.max_cmdline_args   = 16;

    esp_err_t console_err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &s_repl);
    if (console_err != ESP_OK) {
        /* Fatal: no console to report errors and restarting on a persistent
         * console failure creates an infinite boot loop. Halt instead. */
        ESP_LOGE(TAG, "console init failed (%s); halting boot", esp_err_to_name(console_err));
        return;
    }

    /* Boot step 12: Register help command. */
    esp_err_t help_err = esp_console_register_help_command();
    if (help_err != ESP_OK) {
        ESP_LOGE(TAG, "help command registration failed (%s)", esp_err_to_name(help_err));
    }

    /* Boot step 13: Register CLI commands. */
    esp_err_t reg_err = net_cli_register_all();
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "command registration failed (%s)", esp_err_to_name(reg_err));
    }

    /* Boot step 13.5: Register "fwupgrade" (NETSHELL32-2, FR-901/902/904).
     * A failure here must not prevent the REPL from starting or the
     * other commands from registering. */
    esp_err_t ble_ota_reg_err = ble_ota_register_all();
    if (ble_ota_reg_err != ESP_OK) {
        ESP_LOGE(TAG, "ble_ota command registration failed (%s)", esp_err_to_name(ble_ota_reg_err));
    }

    /* Boot step 13.6 (NETSHELL32-5): the five diagnostic commands plus the
     * serial-only hw_rev_set on the existing console. A failure must not
     * prevent the REPL from starting. */
    esp_err_t diag_reg_err = diag_cmds_register_serial();
    if (diag_reg_err != ESP_OK) {
        ESP_LOGE(TAG, "diag command registration failed (%s)", esp_err_to_name(diag_reg_err));
    }
#if CONFIG_NS5_TEST_STALL_HOST
    const esp_console_cmd_t stall_cmd = {
        .command = "ns5_stall",
        .help = "TEST BUILD ONLY: block the BLE host task for <ms>",
        .hint = NULL,
        .func = &cmd_ns5_stall,
        .argtable = NULL,
    };
    (void)esp_console_cmd_register(&stall_cmd);
#endif

    /* Boot step 14: Print degraded-boot banner (if any boot steps failed).
     * Printed before REPL start to avoid race with the first prompt. */
    net_cli_print_boot_banner();

    /* Boot step 15: Start the REPL. */
    esp_err_t start_err = esp_console_start_repl(s_repl);

    /* Boot step 15.5: Record boot-status facts for ble_ota's health
     * checks H-1 (console/REPL up) and H-3 (Wi-Fi STA initialised,
     * tech_spec.md §7.2). Recorded regardless of start_err so a REPL
     * start failure is itself visible to H-1. NFR-303 forbids adding a
     * getter to net_cli's private boot-status accessor, and net_cli.h
     * exposes none, so this is the only source for these two facts. */
    ble_ota_note_boot_status(st.wifi_init, st.wifi_set_mode, st.wifi_start,
                              console_err, start_err);

    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed (%s); halting boot", esp_err_to_name(start_err));
        return;
    }

    /* Boot step 15.6 (NETSHELL32-5, last step): bring up the BLE host. It runs
     * AFTER the REPL and after the H-1/H-3 facts above are recorded, so a slow
     * or wedged controller bring-up (nimble_port_init has no timeout) can only
     * block this task, which has nothing left to do, and can never fail a health
     * check. Skipped if the console or REPL failed (returned above) or if
     * ble_host_init() failed. */
    if (host_init_err == ESP_OK) {
        esp_err_t host_start_err = ble_host_start();
        if (host_start_err != ESP_OK) {
            ESP_LOGE(TAG, "ble_host_start failed (%s); BLE unavailable, serial unaffected",
                     esp_err_to_name(host_start_err));
        }
    }
    /* Test hook H-4 / V-06: one HEAP_STAT plus STACK_HWM for every task, including
     * the main task right at the end of ble_host_start(). */
    diag_resmon_emit("host_started");

#if CONFIG_NS5_TEST_AUTORUN
    if (xTaskCreate(ns5_autorun_task, "ns5_autorun", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "autorun task creation failed");
    }
#endif
}
