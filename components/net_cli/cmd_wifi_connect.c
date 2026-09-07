/**
 * @file cmd_wifi_connect.c
 * @brief wifi_connect command: join a Wi-Fi network with SSID and optional password
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
 * Credential copy uses length-validated memcpy instead of strncpy to avoid
 * -Wstringop-truncation warnings while safely handling 32-byte SSIDs.
 * Password is never echoed in any form, not even its length.
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "argtable3/argtable3.h"
#include "net_cli_internal.h"

/* Argtable structures allocated once at registration, never freed per
 * invocation: freeing on a parse-error path would be a use-after-free,
 * since the console holds this struct for the program's lifetime. */
static struct {
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_connect_args;

/* No public wifi_err_reason_t-to-string helper exists in v6.0.2, so we
 * provide our own mapping for common disconnection reasons. */
static const char *net_cli_wifi_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:            return "WIFI_REASON_AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:             return "WIFI_REASON_AUTH_LEAVE";
    case WIFI_REASON_ASSOC_LEAVE:            return "WIFI_REASON_ASSOC_LEAVE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:            return "WIFI_REASON_NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:              return "WIFI_REASON_AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:             return "WIFI_REASON_ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "WIFI_REASON_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:        return "WIFI_REASON_CONNECTION_FAIL";
    default:                                 return "unknown";
    }
}

static int cmd_wifi_connect_handler(int argc, char **argv)
{
    if (!net_cli_wifi_ready()) {
        printf("wifi_connect: Wi-Fi subsystem unavailable (see boot banner)\n");
        return 1;
    }

    /* SSID is mandatory; parse errors return before touching Wi-Fi state. */
    int nerrors = arg_parse(argc, argv, (void **)&s_connect_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_connect_args.end, argv[0]);
        return 1;
    }

    const char *ssid = s_connect_args.ssid->sval[0];
    const char *pass = (s_connect_args.pass->count > 0) ? s_connect_args.pass->sval[0] : "";

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);

    /* Reject over-length credentials; never truncate. */
    if (ssid_len == 0) {
        printf("wifi_connect: SSID must not be empty\n");
        return 1;
    }
    if (ssid_len > 32) {
        printf("wifi_connect: SSID too long (%u bytes, max 32)\n", (unsigned)ssid_len);
        return 1;
    }
    if (pass_len > 64) {
        printf("wifi_connect: password too long (%u bytes, max 64)\n", (unsigned)pass_len);
        return 1;
    }

    wifi_config_t cfg = { 0 };   /* Zero-initialize to ensure no stale fields. */

    /* Redundant bounds-checking here helps the compiler prove memcpy lengths
     * are bounded, with no reliance on value-range propagation past the
     * earlier length checks. */
    size_t ssid_n = (ssid_len <= sizeof(cfg.sta.ssid))     ? ssid_len : sizeof(cfg.sta.ssid);
    size_t pass_n = (pass_len <= sizeof(cfg.sta.password)) ? pass_len : sizeof(cfg.sta.password);

    memcpy(cfg.sta.ssid, ssid, ssid_n);
    memcpy(cfg.sta.password, pass, pass_n);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* Accept open networks; no minimum. */

    /* Print feedback before the full connection sequence begins, so the
     * operator sees progress before the ~200ms settle time. */
    printf("Connecting to \"%s\"...\n", ssid);

    net_cli_connect_result_t result = { 0 };
    esp_err_t attempt_err = net_cli_wifi_connect_attempt(&cfg, NET_CLI_WIFI_CONNECT_MS, &result);
    if (attempt_err != ESP_OK) {
        printf("wifi_connect: unable to start connection attempt (%s)\n",
               esp_err_to_name(attempt_err));
        return 1;
    }

    switch (result.outcome) {
    case NET_CLI_CONNECT_CONNECTED: {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = { 0 };
        if (nif != NULL) {
            /* Zero-initialised ip renders 0.0.0.0 if this fails, which is
             * not expected here since CONNECTED already implies GOT_IP. */
            esp_err_t ip_err = esp_netif_get_ip_info(nif, &ip);
            (void)ip_err;
        }
        printf("Connected. IP: " IPSTR "\n", IP2STR(&ip.ip));
        return 0;
    }
    case NET_CLI_CONNECT_FAILED:
        printf("Failed (reason %u: %s)\n", (unsigned)result.reason,
               net_cli_wifi_reason_str(result.reason));
        return 1;
    case NET_CLI_CONNECT_TIMEOUT:
    default:
        /* Timed out but did not disconnect; association may still complete. */
        printf("Timeout (association may still be completing - run ifconfig to check)\n");
        return 1;
    }
}

esp_err_t net_cli_register_wifi_connect(void)
{
    s_connect_args.ssid = arg_str1("s", NULL, "<ssid>", "SSID of the network to join");
    s_connect_args.pass = arg_str0("p", NULL, "<password>", "Password (omit for an open network)");
    s_connect_args.end  = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "wifi_connect",
        .help = "Connect the station interface to a Wi-Fi network",
        .hint = NULL,
        .func = &cmd_wifi_connect_handler,
        .argtable = &s_connect_args,
    };
    return esp_console_cmd_register(&cmd);
}
