/**
 * @file cmd_ifconfig.c
 * @brief ifconfig command: display station interface IP config, MAC address, and link status
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
 * Unlike wifiscan/wifi_connect/ping, this command does NOT gate on
 * net_cli_wifi_ready(): it degrades instead so it remains functional even
 * when boot came up degraded -- making it the most useful diagnostic tool
 * for troubleshooting.
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "net_cli_internal.h"

static int cmd_ifconfig_handler(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("ifconfig: usage: ifconfig\n");
        return 1;
    }

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        printf("ifconfig: station interface not found (WIFI_STA_DEF)\n");
        return 1;
    }

    /* On failure, ensure all-zero ip_info so IP/mask/gateway render as
     * 0.0.0.0 rather than stale/uninitialized values. */
    esp_netif_ip_info_t ip = { 0 };
    esp_err_t ip_err = esp_netif_get_ip_info(nif, &ip);
    if (ip_err != ESP_OK) {
        printf("ifconfig: failed to read IP info (%s)\n", esp_err_to_name(ip_err));
        memset(&ip, 0, sizeof(ip));
    }

    /* MAC query is independent of Wi-Fi association state. */
    uint8_t mac[6] = { 0 };
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);

    /* Connected iff IP is non-zero. */
    bool connected = (ip.ip.addr != 0);

    printf("Interface:   WIFI_STA_DEF\n");
    printf("IP address:  " IPSTR "\n", IP2STR(&ip.ip));
    printf("Subnet mask: " IPSTR "\n", IP2STR(&ip.netmask));
    printf("Gateway:     " IPSTR "\n", IP2STR(&ip.gw));

    if (mac_err == ESP_OK) {
        printf("MAC address: " NET_CLI_MACSTR "\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        /* A real failure must never be mistaken for a valid all-zero MAC. */
        printf("MAC address: <unavailable (%s)>\n", esp_err_to_name(mac_err));
    }

    printf("Status: %s\n", connected ? "Connected" : "Disconnected");

    return 0;
}

esp_err_t net_cli_register_ifconfig(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ifconfig",
        .help = "Show the station interface's IP configuration, MAC address and link status",
        .hint = NULL,
        .func = &cmd_ifconfig_handler,
        .argtable = NULL,
    };
    return esp_console_cmd_register(&cmd);
}
