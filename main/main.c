/*
 * main.c -- boot sequence only (FR-1..FR-12). No command logic, no
 * printf of command output, no argtable declaration, and no esp_wifi_*
 * call beyond FR-5/FR-6/FR-7 -- this is the mechanical check that
 * Approach B's component boundary held (tech_spec.md §5.1).
 *
 * The boot cascade below is CONDITIONAL, not a flat sequence of calls
 * (tech_spec.md §6.1.1 -- binding). esp_netif_create_default_wifi_sta()
 * aborts (via assert()/ESP_ERROR_CHECK()) if esp_netif_init() or
 * esp_event_loop_create_default() failed first (V6-6), so each Tier-2 step
 * below only runs if the step(s) it depends on already recorded ESP_OK. A
 * step that is skipped records NET_CLI_ERR_SKIPPED, never ESP_OK.
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

/* Held for the lifetime of the program (never torn down); static file-scope,
 * not a global (NFR-22). */
static esp_console_repl_t *s_repl = NULL;

/* FR-1 / FR-1.1 / OQ-6 (§8.2): standard erase-and-retry idiom, retried at
 * most once. nvs_flash_erase()'s own return is checked rather than assumed
 * (NFR-15) -- the common ESP_ERROR_CHECK() idiom would abort(), which
 * NFR-16/§8.3 prohibit. */
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
    net_cli_boot_status_t st = { 0 };   /* stack local; copied by value at step 9 (DA-10) */

    /* ---- Boot step 1: NVS ---- */
    st.nvs_init = boot_nvs();

    /* ---- Boot step 2: netif ---- */
    st.netif_init = esp_netif_init();

    /* ---- Boot step 3: default event loop ---- */
    st.event_loop = esp_event_loop_create_default();

    bool netif_ok = (st.netif_init == ESP_OK);
    bool loop_ok  = (st.event_loop == ESP_OK);

    /* ---- Boot step 4: default Wi-Fi STA netif [COND: 2 && 3] ----
     * V6-6: this call aborts rather than returning an error if either
     * prerequisite is missing, so it must not be called unless both
     * already succeeded (DA-1 / §6.1.1). */
    if (netif_ok && loop_ok) {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        st.netif_create_sta = (sta != NULL) ? ESP_OK : ESP_FAIL;
    } else {
        st.netif_create_sta = NET_CLI_ERR_SKIPPED;
    }

    /* ---- Boot step 5: esp_wifi_init [COND: 4] ---- */
    if (st.netif_create_sta == ESP_OK) {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        st.wifi_init = esp_wifi_init(&wifi_cfg);
    } else {
        st.wifi_init = NET_CLI_ERR_SKIPPED;
    }

    /* ---- Boot step 6: esp_wifi_set_mode [COND: 5] ---- */
    st.wifi_set_mode = (st.wifi_init == ESP_OK) ? esp_wifi_set_mode(WIFI_MODE_STA)
                                                 : NET_CLI_ERR_SKIPPED;

    /* ---- Boot step 7: net_cli_init [COND: 3] ----
     * Depends on the event loop only -- the sync primitives are still
     * worth creating even when Wi-Fi itself is dead. Registering the
     * WIFI_EVENT/IP_EVENT handler here, before esp_wifi_start() (step 8),
     * guarantees no association-related event can be missed. */
    st.cli_init = loop_ok ? net_cli_init() : NET_CLI_ERR_SKIPPED;

    /* ---- Boot step 8: esp_wifi_start [COND: 6] ---- */
    st.wifi_start = (st.wifi_set_mode == ESP_OK) ? esp_wifi_start()
                                                  : NET_CLI_ERR_SKIPPED;

    /* ---- Boot step 9: publish the boot-status record ----
     * net_cli_set_boot_status() copies *st BY VALUE (DA-10); st itself is
     * reclaimed once app_main() returns after step 15. */
    net_cli_set_boot_status(&st);

    /* ---- Boot step 10: log-level policy (OQ-11 / §8.6) ----
     * Before the REPL is created, so no noisy Wi-Fi/netif log line can
     * land between the boot banner and the first prompt. */
    esp_log_level_set("wifi",               ESP_LOG_WARN);
    esp_log_level_set("wifi_init",          ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip",     ESP_LOG_WARN);
    esp_log_level_set("phy_init",           ESP_LOG_WARN);

    /* ---- Boot step 11: REPL over UART0 (FR-9, HW-4, HW-5) ---- */
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    uart_cfg.channel     = 0;         /* UART0 -- HW-4 */
    uart_cfg.baud_rate   = 115200;    /* 8N1 -- HW-5 */
    uart_cfg.tx_gpio_num = -1;        /* board default pins -- HW-6 */
    uart_cfg.rx_gpio_num = -1;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt             = "esp32_net>";   /* NO trailing space -- V6-3 */
    repl_cfg.max_history_len    = 16;
    repl_cfg.history_save_path  = NULL;
    repl_cfg.task_stack_size    = 8192;
    repl_cfg.task_priority      = 2;
    repl_cfg.task_core_id       = tskNO_AFFINITY;
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.max_cmdline_args   = 16;

    esp_err_t console_err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &s_repl);
    if (console_err != ESP_OK) {
        /* Tier 1 fatal (§8.3): there is no console on which to report
         * anything, and restarting on a deterministic console failure is
         * an infinite boot loop -- strictly worse for diagnosis than a
         * halted board with one log line. Never abort()/esp_restart(). */
        ESP_LOGE(TAG, "console init failed (%s); halting boot", esp_err_to_name(console_err));
        return;
    }

    /* ---- Boot step 12: help command (FR-18) ---- */
    esp_err_t help_err = esp_console_register_help_command();
    if (help_err != ESP_OK) {
        ESP_LOGE(TAG, "help command registration failed (%s)", esp_err_to_name(help_err));
    }

    /* ---- Boot step 13: register net_cli's four commands (FR-10) ---- */
    esp_err_t reg_err = net_cli_register_all();
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "command registration failed (%s)", esp_err_to_name(reg_err));
    }

    /* ---- Boot step 14: degraded-boot banner (FR-11) ----
     * Printed after registration and before start_repl(), so it never
     * races the first prompt (NFR-4). */
    net_cli_print_boot_banner();

    /* ---- Boot step 15: start the REPL (FR-17) ---- */
    esp_err_t start_err = esp_console_start_repl(s_repl);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed (%s); halting boot", esp_err_to_name(start_err));
        return;
    }
}
