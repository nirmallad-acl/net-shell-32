/**
 * @file cmd_wifiscan.c
 * @brief wifiscan command: scan for and display nearby Wi-Fi networks
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
 * CRITICAL: Every early-return path must call esp_wifi_clear_ap_list(),
 * because esp_wifi_scan_get_ap_records() only releases the driver-internal
 * AP list on the path that actually reaches it. Missing any one of these
 * calls is a driver-heap leak that free(recs) discipline alone cannot catch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "net_cli_internal.h"

_Static_assert(NET_CLI_SCAN_MAX_APS * sizeof(wifi_ap_record_t) <= 4096,
               "wifiscan record buffer exceeds its 4 KB design budget");

static int cmd_wifiscan_handler(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("wifiscan: usage: wifiscan\n");
        return 1;
    }

    if (!net_cli_wifi_ready()) {
        printf("wifiscan: Wi-Fi subsystem unavailable (see boot banner)\n");
        return 1;
    }

    wifi_scan_config_t scan_cfg = { 0 };
    scan_cfg.show_hidden = true;   /* Include hidden networks. */

    /* Blocking scan; surface driver errors verbatim, never retry. */
    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_err != ESP_OK) {
        printf("wifiscan: scan failed (%s)\n", esp_err_to_name(scan_err));
        return 1;
    }

    uint16_t found = 0;
    esp_err_t num_err = esp_wifi_scan_get_ap_num(&found);
    if (num_err != ESP_OK) {
        printf("wifiscan: failed to read AP count (%s)\n", esp_err_to_name(num_err));
        esp_wifi_clear_ap_list();
        return 1;
    }

    if (found == 0) {
        printf("No networks found.\n");
        esp_wifi_clear_ap_list();
        return 0;
    }

    /* Clamp to max APs we can display. esp_wifi_scan_get_ap_records() frees
     * the ENTIRE driver-side list regardless of how many we request. */
    uint16_t want = (found > NET_CLI_SCAN_MAX_APS) ? (uint16_t)NET_CLI_SCAN_MAX_APS : found;

    /* Use calloc to ensure unpopulated fields read as zero, not garbage
     * (cheap insurance for the %s in the print loop). */
    wifi_ap_record_t *recs = calloc(want, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        printf("wifiscan: out of memory (%u records)\n", (unsigned)want);
        esp_wifi_clear_ap_list();
        return 1;
    }

    uint16_t got = want;   /* in/out parameter */
    esp_err_t rec_err = esp_wifi_scan_get_ap_records(&got, recs);
    if (rec_err != ESP_OK) {
        printf("wifiscan: failed to read AP records (%s)\n", esp_err_to_name(rec_err));
        free(recs);
        recs = NULL;
        /* Defensively clear the driver-side list even on error; the exact
         * driver behavior is unspecified, so this guards against leaks. */
        esp_wifi_clear_ap_list();
        return 1;
    }

    /* Column-aligned header with rule line. Build the rule via precision
     * so its length is provably exact, not hand-counted. */
    static const char k_pad[] = "----------------------------------------";  /* >=32 chars */
    printf("%-32s  %5s  %3s\n", "SSID", "RSSI", "CH");
    printf("%.*s  %.*s  %.*s\n", 32, k_pad, 5, k_pad, 3, k_pad);

    for (uint16_t i = 0; i < got; i++) {
        /* Copy exactly 32 bytes and terminate explicitly; print with
         * explicit precision to prevent over-reading the SSID buffer. */
        char ssid_buf[33];
        memcpy(ssid_buf, recs[i].ssid, 32);
        ssid_buf[32] = '\0';
        if (ssid_buf[0] == '\0') {
            strcpy(ssid_buf, "<hidden>");   /* Safe: 33-byte buffer for 8-byte literal. */
        }
        printf("%-32.32s  %5d  %3u\n", ssid_buf, recs[i].rssi, (unsigned)recs[i].primary);
    }

    if (found > got) {
        printf("Note: %u APs found, showing the strongest %u (limit %u).\n",
               (unsigned)found, (unsigned)got, (unsigned)NET_CLI_SCAN_MAX_APS);
    }

    printf("%u network(s) found.\n", (unsigned)found);

    free(recs);
    recs = NULL;
    return 0;
}

esp_err_t net_cli_register_wifiscan(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifiscan",
        .help = "Scan for nearby Wi-Fi networks and list SSID, RSSI and channel",
        .hint = NULL,
        .func = &cmd_wifiscan_handler,
        .argtable = NULL,
    };
    return esp_console_cmd_register(&cmd);
}
