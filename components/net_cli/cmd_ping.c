/**
 * @file cmd_ping.c
 * @brief ping command: send ICMP echo requests to an IPv4 address or hostname
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
 * IMPORTANT: Does NOT use the fire-and-forget IDF example pattern. Must
 * follow an explicit stop/join/delete handshake, because esp_ping_stop()
 * and esp_ping_delete_session() do not join or free anything themselves --
 * all freeing happens on the ping task after callbacks complete. Leaking
 * this step causes a task + socket + config leak per invocation.
 *
 * Uses "ping/ping_sock.h" from lwip component; no esp_ping component exists.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_console.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "net_cli_internal.h"

static struct {
    struct arg_str *host;
    struct arg_end *end;
} s_ping_args;

/* esp_ping callbacks -- run on the esp_ping task, NOT the REPL task.
 * printf here is acceptable: event-handler constraints apply to Wi-Fi/IP
 * events only, not to ping task output. */

static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    net_cli_ping_state_t *st = (net_cli_ping_state_t *)args;
    if (st->abandoned) {
        return;   /* Silenced session -- checked first so stale callbacks produce no output. */
    }

    uint16_t   seqno = 0;
    uint8_t    ttl = 0;
    uint32_t   recv_len = 0;
    uint32_t   timegap_ms = 0;
    ip_addr_t  target = { 0 };

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,   &seqno,      sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,     &ttl,        sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,    &recv_len,   sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &timegap_ms, sizeof(timegap_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,  &target,     sizeof(target));

    printf("%" PRIu32 " bytes from %s icmp_seq=%" PRIu16 " ttl=%" PRIu8 " time=%" PRIu32 " ms\n",
           recv_len, ipaddr_ntoa(&target), seqno, ttl, timegap_ms);
}

static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    net_cli_ping_state_t *st = (net_cli_ping_state_t *)args;
    if (st->abandoned) {
        return;
    }

    uint16_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    printf("Request timeout for icmp_seq %" PRIu16 "\n", seqno);
}

static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    net_cli_ping_state_t *st = (net_cli_ping_state_t *)args;

    if (!st->abandoned) {
        uint32_t  transmitted = 0;
        uint32_t  received = 0;
        uint32_t  duration_ms = 0;
        ip_addr_t target = { 0 };

        esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,  &transmitted, sizeof(transmitted));
        esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,    &received,    sizeof(received));
        esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));
        esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,   &target,      sizeof(target));

        /* Integer arithmetic only to avoid soft-float overhead in this task's
         * stack budget. transmitted > 0 guards the divide. */
        uint32_t loss = (transmitted > 0)
                         ? (uint32_t)(((transmitted - received) * 100u) / transmitted)
                         : 0u;

        printf("--- %s ping statistics ---\n", ipaddr_ntoa(&target));
        printf("%" PRIu32 " packets transmitted, %" PRIu32 " received, %" PRIu32
               "%% packet loss, time %" PRIu32 " ms\n",
               transmitted, received, loss, duration_ms);
    }

    /* ALWAYS give the semaphore, even on the abandoned path. This is the
     * join signal the stop/join handshake relies on; suppressing it would
     * strand the reclaim gate forever. Must be LAST so all output is flushed
     * before the REPL task can resume. */
    xSemaphoreGive(net_cli_ping_done_sem());
}

/* Target resolution: try numeric literal first, then DNS fallback. */

static int resolve_target(const char *host, ip_addr_t *target)
{
    if (ipaddr_aton(host, target) != 0) {
        return 0;   /* Numeric literal; done. */
    }

    /* Not a literal; attempt DNS resolution (IPv4 only). */
    struct addrinfo hint = { .ai_family = AF_INET, .ai_socktype = SOCK_RAW };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, NULL, &hint, &res);
    if (gai != 0 || res == NULL) {
        printf("ping: cannot resolve host \"%s\"\n", host);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return -1;
    }

    struct in_addr a4 = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(target), &a4);
    target->type = IPADDR_TYPE_V4;
    freeaddrinfo(res);
    return 0;
}

/* Reclaim gate: wait for the previous session's callback phase to complete
 * before allowing a new session. This prevents late semaphore gives from
 * poisoning the next invocation's wait. */

static int ping_reclaim_previous_session(void)
{
    net_cli_ping_state_t *st = net_cli_ping_state();
    if (!st->abandoned) {
        return 0;
    }

    if (xSemaphoreTake(net_cli_ping_done_sem(), pdMS_TO_TICKS(NET_CLI_PING_JOIN_MS)) == pdTRUE) {
        st->abandoned = false;
        return 0;
    }

    printf("ping: previous session is still finishing; try again\n");
    return -1;   /* Bounded, diagnosable error: user can retry. */
}

static int cmd_ping_handler(int argc, char **argv)
{
    if (!net_cli_wifi_ready()) {
        printf("ping: Wi-Fi subsystem unavailable (see boot banner)\n");
        return 1;
    }

    /* Check reclaim gate first: cheapest check, only about our own state. */
    if (ping_reclaim_previous_session() != 0) {
        return 1;
    }

    if (!net_cli_sync_ready()) {
        printf("ping: CLI sync primitives unavailable (see boot banner)\n");
        return 1;
    }

    int nerrors = arg_parse(argc, argv, (void **)&s_ping_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_ping_args.end, argv[0]);
        return 1;
    }
    const char *host = s_ping_args.host->sval[0];

    /* Station must have an IP before attempting name resolution; this gives
     * an immediate, accurate error for the disconnected case instead of
     * sending a hostname into getaddrinfo() with no hope of success. */
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t sta_ip = { 0 };
    if (nif == NULL || esp_netif_get_ip_info(nif, &sta_ip) != ESP_OK || sta_ip.ip.addr == 0) {
        printf("ping: no network connection (station has no IP address)\n");
        return 1;
    }

    ip_addr_t target = { 0 };
    if (resolve_target(host, &target) != 0) {
        return 1;
    }

    net_cli_ping_state_t *st       = net_cli_ping_state();
    SemaphoreHandle_t     done_sem = net_cli_ping_done_sem();

    st->abandoned = false;
    xSemaphoreTake(done_sem, 0);   /* Drain any leftover give from a prior invocation. */

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr     = target;
    cfg.count           = NET_CLI_PING_COUNT;       /* Override default of 5. */
    cfg.task_stack_size = NET_CLI_PING_TASK_STACK;  /* Override default ESP_TASK_PING_STACK. */

    esp_ping_callbacks_t cbs = {
        .cb_args         = st,
        .on_ping_success = &cmd_ping_on_ping_success,
        .on_ping_timeout = &cmd_ping_on_ping_timeout,
        .on_ping_end     = &cmd_ping_on_ping_end,
    };

    printf("PING %s: %" PRIu32 " packets, %" PRIu32 " data bytes\n",
           ipaddr_ntoa(&target), cfg.count, cfg.data_size);

    esp_ping_handle_t hdl = NULL;
    esp_err_t new_err = esp_ping_new_session(&cfg, &cbs, &hdl);
    if (new_err != ESP_OK) {
        printf("ping: failed to create session (%s)\n", esp_err_to_name(new_err));
        return 1;   /* no hdl exists -- nothing to delete */
    }

    esp_err_t start_err = esp_ping_start(hdl);
    if (start_err != ESP_OK) {
        printf("ping: failed to start session (%s)\n", esp_err_to_name(start_err));
        esp_ping_delete_session(hdl);
        return 1;
    }

    bool ok = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(NET_CLI_PING_WAIT_MS)) == pdTRUE);
    if (!ok) {
        /* Set abandoned FIRST to silence callbacks before even calling stop(),
         * so no in-flight success packets print after our timeout message. */
        st->abandoned = true;
        esp_ping_stop(hdl);   /* Async flag write only; does not join. */
        printf("ping: timed out waiting for the session to finish (aborted)\n");

        /* Explicit JOIN: the ping task still owes us one on_ping_end unconditionally.
         * This join is necessary because esp_ping_stop() itself does not join. */
        bool joined = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(NET_CLI_PING_JOIN_MS)) == pdTRUE);
        if (joined) {
            st->abandoned = false;
        } else {
            printf("ping: session abandoned; the next ping may be delayed\n");
        }
    }

    /* Final cleanup: delete the ping session. This is the last use of hdl
     * and must always be called; the ping task cannot exit without it. */
    esp_ping_delete_session(hdl);
    hdl = NULL;

    return ok ? 0 : 1;
}

esp_err_t net_cli_register_ping(void)
{
    s_ping_args.host = arg_str1("h", NULL, "<host_ip_or_domain>", "Target IPv4 address or hostname");
    s_ping_args.end  = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "ping",
        .help = "Send 4 ICMP echo requests to a host and report the result",
        .hint = NULL,
        .func = &cmd_ping_handler,
        .argtable = &s_ping_args,
    };
    return esp_console_cmd_register(&cmd);
}
