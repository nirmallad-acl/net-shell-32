/**
 * @file net_cli.c
 * @brief Central CLI initialization, command registration, and boot banner
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
 * Aggregates command registration, boot banner printing, and state
 * initialization. The boot-status record is owned by net_cli_wifi_state.c.
 */
#include <stdio.h>
#include "esp_err.h"
#include "net_cli.h"
#include "net_cli_internal.h"

esp_err_t net_cli_init(void)
{
    return net_cli_wifi_state_init();
}

esp_err_t net_cli_register_all(void)
{
    esp_err_t err = net_cli_register_ifconfig();
    if (err != ESP_OK) {
        return err;
    }

    err = net_cli_register_wifiscan();
    if (err != ESP_OK) {
        return err;
    }

    err = net_cli_register_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    return net_cli_register_ping();
}

/* One row per boot-status field, in app_main()'s boot-step order, so the
 * root-cause failure is always the first non-OK line and everything below
 * it reads as a "skipped (prerequisite failed)" consequence (§6.1.1/§8.3). */
static void print_boot_field(const char *name, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    if (err == NET_CLI_ERR_SKIPPED) {
        printf("  %s: skipped (prerequisite failed)\n", name);
    } else {
        printf("  %s: %s\n", name, esp_err_to_name(err));
    }
}

void net_cli_print_boot_banner(void)
{
    const net_cli_boot_status_t *st = net_cli_boot_status_get();

    bool all_ok = (st->nvs_init == ESP_OK) &&
                  (st->netif_init == ESP_OK) &&
                  (st->event_loop == ESP_OK) &&
                  (st->netif_create_sta == ESP_OK) &&
                  (st->wifi_init == ESP_OK) &&
                  (st->wifi_set_mode == ESP_OK) &&
                  (st->cli_init == ESP_OK) &&
                  (st->wifi_start == ESP_OK);

    if (all_ok) {
        return;
    }

    printf("WARNING: degraded boot - some commands will be unavailable:\n");
    print_boot_field("nvs_init", st->nvs_init);
    print_boot_field("netif_init", st->netif_init);
    print_boot_field("event_loop", st->event_loop);
    print_boot_field("netif_create_sta", st->netif_create_sta);
    print_boot_field("wifi_init", st->wifi_init);
    print_boot_field("wifi_set_mode", st->wifi_set_mode);
    print_boot_field("cli_init", st->cli_init);
    print_boot_field("wifi_start", st->wifi_start);
}
