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
#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "net_cli.h"

static const char *TAG = "main";

/* REPL: held for the program lifetime, never torn down. */
static esp_console_repl_t *s_repl = NULL;

/* Standard NVS erase-and-retry: if init fails with "no free pages" or
 * "new version", erase and try once more. Checked rather than assumed
 * to avoid aborting on a diagnostic error. */
static esp_err_t boot_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s); erasing and retrying", esp_err_to_name(err));
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

    /* Boot step 14: Print degraded-boot banner (if any boot steps failed).
     * Printed before REPL start to avoid race with the first prompt. */
    net_cli_print_boot_banner();

    /* Boot step 15: Start the REPL. */
    esp_err_t start_err = esp_console_start_repl(s_repl);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed (%s); halting boot", esp_err_to_name(start_err));
        return;
    }
}
