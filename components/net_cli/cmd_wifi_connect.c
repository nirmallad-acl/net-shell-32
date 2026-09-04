/*
 * cmd_wifi_connect.c -- FR-34..FR-41, IMP-12..IMP-15.
 *
 * Credential copy uses length-validated memcpy, not strncpy (DV-2 / GATE 2
 * R-4 -- approved): a literal strncpy(dst, src, sizeof(dst)) risks
 * -Wstringop-truncation under NFR-21's zero-warning gate, and the
 * sizeof(dst)-1 workaround would truncate a legitimate 32-byte SSID.
 *
 * NFR-24: the password is never echoed, in any form -- not even its
 * length.
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "argtable3/argtable3.h"
#include "net_cli_internal.h"

/* DV-3 / GATE 2 R-5 (approved): allocated once at registration, never freed
 * per invocation -- freeing on a parse-error path would be a
 * use-after-free, since the console references this struct for the
 * program's lifetime (§7.0 rule 3). */
static struct {
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_connect_args;

/* V6-5: no public wifi_err_reason_t-to-string helper exists in v6.0.2.
 * WIFI_REASON_ASSOC_EXPIRE is NOT a case here: the tech spec's candidate
 * list included it, but it does not exist in the installed v6.0.2
 * esp_wifi_types_generic.h (verified by grep) -- dropped per the spec's own
 * instruction to confirm each label against the installed header rather
 * than guess (§7.3.4). */
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

    /* arg_str1("s", ...) makes -s mandatory: on a parse error we return
     * before touching any Wi-Fi state -- FR-35, no config write, no
     * connect attempt. */
    int nerrors = arg_parse(argc, argv, (void **)&s_connect_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_connect_args.end, argv[0]);
        return 1;   /* argtable owns its own storage -- nothing to free here (DV-3) */
    }

    const char *ssid = s_connect_args.ssid->sval[0];
    const char *pass = (s_connect_args.pass->count > 0) ? s_connect_args.pass->sval[0] : "";  /* FR-36 */

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);

    /* OQ-8 (§8.4): reject, never truncate. */
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

    wifi_config_t cfg = { 0 };   /* zero-initialised -- IMP-15 */

    /* Redundant re-clamp (DA-7): the checks above already returned on an
     * over-length value, so this is dead code at runtime and free at
     * compile time (GCC folds it once the range is known) -- but it keeps
     * each memcpy's length statically bounded by the destination size at
     * the call site, with no reliance on value-range propagation crossing
     * the earlier `return 1` guard under -Wstringop-overflow. */
    size_t ssid_n = (ssid_len <= sizeof(cfg.sta.ssid))     ? ssid_len : sizeof(cfg.sta.ssid);
    size_t pass_n = (pass_len <= sizeof(cfg.sta.password)) ? pass_len : sizeof(cfg.sta.password);

    memcpy(cfg.sta.ssid, ssid, ssid_n);         /* DV-2 -- ssid_n <= 32, twice-bounded */
    memcpy(cfg.sta.password, pass, pass_n);     /* DV-2 -- pass_n <= 64, twice-bounded */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* open networks work; no forced minimum */

    /* DA-12: printed BEFORE the §6.4 sequence begins, not just before the
     * connect call, so the operator sees feedback before the ~200 ms
     * disarm/settle prelude. */
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
        /* DV-9: qualified rather than a bare "Timeout" -- DA-3 means the
         * association is left untouched and may complete moments later. */
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
