/*
 * cmd_ifconfig.c -- FR-21..FR-26, IMP-2..IMP-5.
 *
 * `ifconfig` deliberately does NOT gate on net_cli_wifi_ready() (§7.0 rule 5
 * only applies to wifiscan/wifi_connect/ping): it degrades instead, so it
 * remains the one command that still works -- and is the most useful --
 * when boot came up degraded (OQ-7 / §8.3).
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

    /* IMP-2 */
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        /* IMP-5 -- this is the diagnostic a degraded boot needs most. */
        printf("ifconfig: station interface not found (WIFI_STA_DEF)\n");
        return 1;
    }

    /* IMP-3. On failure, fall through with an all-zero ip_info so IP/mask/
     * gateway render as 0.0.0.0 -- FR-26, no stale/uninitialised values. */
    esp_netif_ip_info_t ip = { 0 };
    esp_err_t ip_err = esp_netif_get_ip_info(nif, &ip);
    if (ip_err != ESP_OK) {
        printf("ifconfig: failed to read IP info (%s)\n", esp_err_to_name(ip_err));
        memset(&ip, 0, sizeof(ip));
    }

    /* OQ-2 (§8.1): MAC always attempted, independent of association state. */
    uint8_t mac[6] = { 0 };
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, mac);

    /* Single source of truth for "connected": IP.addr != 0 (§7.1). */
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

    /* DV-5: printed unconditionally -- a superset of the OQ-3 resolution
     * (which mandated the line only when disconnected); byte-identical in
     * the disconnected case, and gives QA a positive assertion when
     * connected too. */
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
