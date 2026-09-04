/*
 * cmd_wifiscan.c -- FR-27..FR-33, IMP-6..IMP-11.
 *
 * The single easiest mistake in this file: every early-return path (steps
 * 4, 5, 7 below, PLUS step 8's own error branch -- DA-4) must call
 * esp_wifi_clear_ap_list(), because esp_wifi_scan_get_ap_records() only
 * releases the driver-internal AP list on the path that actually reaches it
 * (V6-4). Missing any one of the four is a driver-heap leak that
 * free(recs) discipline alone cannot catch. See tech_spec.md §7.2, R-3.
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
    scan_cfg.show_hidden = true;   /* IMP-6 / FR-31 */

    /* IMP-7 / FR-28: blocking scan. OQ-12 (§8.7): surface driver errors
     * verbatim, never retry, never implicitly disconnect. */
    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_err != ESP_OK) {
        printf("wifiscan: scan failed (%s)\n", esp_err_to_name(scan_err));
        return 1;
    }

    uint16_t found = 0;
    esp_err_t num_err = esp_wifi_scan_get_ap_num(&found);   /* IMP-8 */
    if (num_err != ESP_OK) {
        printf("wifiscan: failed to read AP count (%s)\n", esp_err_to_name(num_err));
        esp_wifi_clear_ap_list();
        return 1;
    }

    if (found == 0) {
        printf("No networks found.\n");   /* FR-32: success, not a failure */
        esp_wifi_clear_ap_list();
        return 0;
    }

    /* OQ-4 clamp. Leak-free by construction on the success path (V6-4):
     * esp_wifi_scan_get_ap_records() frees the WHOLE driver-side list
     * regardless of how few records are requested. */
    uint16_t want = (found > NET_CLI_SCAN_MAX_APS) ? (uint16_t)NET_CLI_SCAN_MAX_APS : found;

    /* calloc, not malloc: any field the driver does not populate reads as
     * zero rather than garbage (cheap insurance for the %s in the print
     * loop below). IMP-9. */
    wifi_ap_record_t *recs = calloc(want, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        printf("wifiscan: out of memory (%u records)\n", (unsigned)want);   /* IMP-11 */
        esp_wifi_clear_ap_list();
        return 1;
    }

    uint16_t got = want;   /* in/out */
    esp_err_t rec_err = esp_wifi_scan_get_ap_records(&got, recs);   /* IMP-9 */
    if (rec_err != ESP_OK) {
        printf("wifiscan: failed to read AP records (%s)\n", esp_err_to_name(rec_err));
        free(recs);
        recs = NULL;
        /* DA-4: not proven by source that a failing call still frees the
         * driver-side list, so clear it defensively -- a no-op on an
         * already-freed list, but the missing case is a real leak. */
        esp_wifi_clear_ap_list();
        return 1;   /* IMP-10 */
    }

    /* FR-29: column-aligned header + rule line. Build the rule from a
     * padding source via precision so its length is provably exact instead
     * of relying on manually counted dashes in a string literal. */
    static const char k_pad[] = "----------------------------------------";  /* >=32 chars */
    printf("%-32s  %5s  %3s\n", "SSID", "RSSI", "CH");
    printf("%.*s  %.*s  %.*s\n", 32, k_pad, 5, k_pad, 3, k_pad);

    for (uint16_t i = 0; i < got; i++) {
        /* FR-33: no over-read even though the driver NUL-terminates ssid --
         * copy exactly 32 bytes, terminate explicitly, print with an
         * explicit precision as a second line of defence (§7.0 rule 7). */
        char ssid_buf[33];
        memcpy(ssid_buf, recs[i].ssid, 32);
        ssid_buf[32] = '\0';
        if (ssid_buf[0] == '\0') {
            strcpy(ssid_buf, "<hidden>");   /* FR-31 -- literal into a 33-byte buffer, safe */
        }
        printf("%-32.32s  %5d  %3u\n", ssid_buf, recs[i].rssi, (unsigned)recs[i].primary);
    }

    if (found > got) {
        printf("Note: %u APs found, showing the strongest %u (limit %u).\n",
               (unsigned)found, (unsigned)got, (unsigned)NET_CLI_SCAN_MAX_APS);
    }

    printf("%u network(s) found.\n", (unsigned)found);

    free(recs);
    recs = NULL;   /* IMP-10 */
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
