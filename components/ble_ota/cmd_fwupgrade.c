/**
 * @file cmd_fwupgrade.c
 * @brief fwupgrade start|stop|status console command
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-09-09
 *
 * @copyright Copyright (c) 2026 ACL Digital Pvt Ltd. All rights reserved.
 *
 * CONFIDENTIALITY NOTICE:
 * This software and documentation are the confidential and proprietary
 * information of ACL Digital Pvt Ltd. Unauthorized copying, distribution,
 * modification, or reverse engineering of this file, via any medium,
 * is strictly prohibited.
 *
 * FR-901..909: registered on the existing shared esp_console REPL. NETSHELL32-5:
 * "start" ARMS the OTA service (the BLE host is owned by ble_host and is up from
 * boot) and "stop" DISARMS it; neither starts or stops BLE (tech_spec.md 2.4.6).
 *
 * tech_spec.md §6.7 (r3, authoritative): Wi-Fi suspension is one-way and
 * never automatically restored. wifi_suspend_hook() below is called by
 * ble_ota_proto.c (L2) exactly once per boot session, at the actual
 * transfer-begin transition (0xFD accepted in PARAMS_SET), not by
 * "fwupgrade start"/"stop" themselves -- net_cli itself is never modified
 * (NFR-303). There is deliberately no restore_wifi()/matching "stop"
 * mechanism: restoring Wi-Fi is a manual operator action via the
 * existing `wifi_connect` command (§6.7.5). This replaces r2's paired
 * suspend/restore callback, withdrawn after four consecutive concurrency
 * defects (code_review.md MAJOR-2..5) -- see tech_spec.md §0.3/§6.7.2.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "ble_ota_flash.h"
#include "ble_ota_health.h"
#include "ble_ota_internal.h"
#include "ble_ota_proto.h"
#include "ble_ota_transport.h"

static const char *TAG = BLE_OTA_TAG_FACADE;

static struct {
    struct arg_str *subcmd;
    struct arg_end *end;
} s_fwupgrade_args;

#if CONFIG_BLE_OTA_SUSPEND_WIFI
static bool           s_wifi_suspended;
static bool           s_wifi_suspend_failed;
static bool           s_wifi_was_connected;
static wifi_config_t  s_wifi_saved_cfg;

/* tech_spec.md §6.7: the one-way Wi-Fi suspend action, registered with
 * ble_ota_proto.c (L2) as the single downward hook it calls exactly once
 * per boot session, at the actual transfer-begin transition (0xFD
 * accepted in PARAMS_SET -> RECEIVING). There is no counterpart "resume"
 * function and none should be added here -- see the file header and
 * tech_spec.md §6.7.2/§6.7.5.
 *
 * §6.7.6: the only protection this needs is local idempotence, because it
 * is invoked from exactly one state-machine transition, itself reachable
 * only from the frame-RX path on the NimBLE host task -- repeat
 * invocations are sequential on a single task, not concurrent, so a plain
 * static bool is the correct and complete guard. No mutex, semaphore,
 * atomic or timeout is required or permitted here (adding one would
 * reintroduce the class of shared synchronisation state that produced
 * code_review.md MAJOR-5). */
static void wifi_suspend_hook(void)
{
    if (s_wifi_suspended) {
        /* §6.7.6 case 1/2: a second 0xFD after an abort, or a second
         * fwupgrade start->transfer cycle, in the same boot session. */
        ESP_LOGI(TAG, "FWUPG_WIFI_SUSPEND_SKIP reason=already_suspended");
        return;
    }

    /* FR-1008 (§6.7.3): emitted before the esp_wifi_* calls below, so a
     * suspend that then fails part-way still leaves the operator warned. */
    ESP_LOGW(TAG, "FWUPG_WIFI_SUSPEND_WARN starting the update turns Wi-Fi off "
                   "and it will NOT come back on automatically -- run "
                   "'wifi_connect' to restore it");

    wifi_ap_record_t ap_info;
    bool was_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    wifi_config_t saved_cfg;
    memset(&saved_cfg, 0, sizeof(saved_cfg));
    (void)esp_wifi_get_config(WIFI_IF_STA, &saved_cfg);

    esp_err_t derr = esp_wifi_disconnect();
    esp_err_t serr = esp_wifi_stop();
    if (derr != ESP_OK || serr != ESP_OK) {
        /* §6.7.6 failure handling: log at ERROR, do NOT mark suspended (so
         * a later attempt may retry rather than being suppressed by the
         * idempotence guard above), and continue the transfer regardless
         * -- this is a DRAM optimisation, not a correctness precondition. */
        s_wifi_suspend_failed = true;
        ESP_LOGE(TAG, "Wi-Fi suspend failed: disconnect=%s stop=%s -- "
                       "continuing transfer without it",
                  esp_err_to_name(derr), esp_err_to_name(serr));
        return;
    }

    s_wifi_was_connected = was_connected;
    s_wifi_saved_cfg = saved_cfg;
    s_wifi_suspended = true;
    s_wifi_suspend_failed = false;
    /* FR-1009 (§6.7.4): distinct confirmation line, only once both calls
     * above have actually returned ESP_OK. */
    ESP_LOGW(TAG, "FWUPG_WIFI_SUSPENDED ssid=%s was_connected=%d",
              (const char *)s_wifi_saved_cfg.sta.ssid, (int)s_wifi_was_connected);
    printf("fwupgrade: Wi-Fi suspended -- it will NOT restore automatically; "
           "run 'wifi_connect' when the update finishes\n");
}
#else
static void wifi_suspend_hook(void)
{
    /* §9/§10.4: CONFIG_BLE_OTA_SUSPEND_WIFI=n means "never suspend at
     * all" -- the literal-NFR-302/NFR-304-compliance escape. Logged so
     * the skip is positively observable rather than silent. */
    ESP_LOGI(TAG, "FWUPG_WIFI_SUSPEND_SKIP reason=disabled_by_kconfig");
}
#endif /* CONFIG_BLE_OTA_SUSPEND_WIFI */

/* One string constant for the stuck-drain operator text; also used verbatim in
 * docs/ble_cli.md (tech_spec.md 2.4.6, G2-07). */
#define DRAIN_STUCK_HINT "reset the device: run 'reboot' (serial or BLE CLI) or press EN"

static int refuse_start(const char *text, const char *outcome)
{
    printf("fwupgrade: cannot start -- %s\n", text);
    ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=%s", outcome);
    return 1;
}

static int do_start(void)
{
    /* tech_spec.md §10.4(2): console commands are commands too -- each
     * logs its invocation and its outcome. */
    ESP_LOGI(TAG, "FWUPG_CMD cmd=start state=invoked");

    bool peer = false;
    esp_err_t ferr = ESP_OK;
    switch (ble_ota_transport_arm(&peer, &ferr)) {
    case OTA_ARM_OK:
        printf("fwupgrade: started (armed) peer_connected=%d\n", peer ? 1 : 0);
        ESP_LOGI(TAG, "FWUPG_CMD cmd=start outcome=ok");
        return 0;

    case OTA_ARM_ALREADY_RUNNING:
        /* FR-905: explicit "already running" error, never a double-init. */
        printf("fwupgrade: already running\n");
        ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=already_running");
        return 1;

    case OTA_ARM_BUSY:
        return refuse_start("a previous stop is still in progress", "busy");

    case OTA_ARM_HOST_STARTING:
        return refuse_start("BLE host is still starting, retry in a few seconds", "host_starting");

    case OTA_ARM_HOST_UNAVAILABLE:
        return refuse_start("BLE host is not available (see BLECLI_HOST_FAIL)", "host_unavailable");

    case OTA_ARM_NOT_REGISTERED:
        return refuse_start("OTA service is not registered (see FWUPG_REGISTER_FAIL)", "ota_not_registered");

    case OTA_ARM_PENDING_VERIFY: {
        uint32_t remaining = ble_ota_health_remaining_deadline_s();
        printf("fwupgrade: cannot start -- device is in post-update health verification\n");
        printf("fwupgrade:   running image is PENDING_VERIFY; OTA is unavailable until it is validated\n");
        printf("fwupgrade:   retry in <=%" PRIu32 " s, or watch for FWUPG_VALIDATED / FWUPG_ROLLBACK_TRIGGERED\n",
               remaining);
        /* tech_spec.md §10.4(4): the pending-verify gate is a decision
         * point in its own right (Finding F-2 / §6.4(2)) -- log that it
         * fired and the remaining-seconds estimate it reported. */
        ESP_LOGW(TAG, "FWUPG_DECIDE point=pending_verify_gate inputs=remaining_s=%" PRIu32 " outcome=refused",
                 remaining);
        ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=refused_pending_verify");
        return 1;
    }

    case OTA_ARM_FLASH_FAIL:
        printf("fwupgrade: failed to start flash worker (%s)\n", esp_err_to_name(ferr));
        ESP_LOGE(TAG, "FWUPG_CMD cmd=start outcome=flash_start_failed err=%s", esp_err_to_name(ferr));
        return 1;

    case OTA_ARM_HOST_TIMEOUT:
        return refuse_start("BLE host did not respond", "host_timeout");

    case OTA_ARM_RES_DRAIN_STUCK:
        printf("fwupgrade: cannot start -- previous OTA drain is stuck; %s (see FWUPG_DRAIN_TIMEOUT)\n",
               DRAIN_STUCK_HINT);
        ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=drain_stuck");
        return 1;

    default:
        printf("fwupgrade: cannot start -- internal error\n");
        ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=internal");
        return 1;
    }
}

/* tech_spec.md §6.7.5 (binding): "fwupgrade stop" disarms OTA (NETSHELL32-5; in
 * NETSHELL32-2 it tore the BLE stack down, FR-103) and nothing more -- it must NEVER
 * restore Wi-Fi. This is
 * explicitly called out as the single most likely place for an
 * implementer or reviewer to "helpfully" add a restore call; doing so
 * re-creates code_review.md MAJOR-4 verbatim, since this handler runs on
 * the REPL task, independently of whichever task ran the suspend. Do not
 * add one here. */
static int do_stop(void)
{
    ESP_LOGI(TAG, "FWUPG_CMD cmd=stop state=invoked");

    /* NETSHELL32-5: "stop" now DISARMS OTA only. The BLE host and the CLI stay up
     * (REQ-INT-01), and the link is released only if it is OTA-only (REQ-FUNC-76).
     * The abort is issued on the host task in the same step that closes the gate;
     * the drain (ble_ota_proto_wait_idle, bounded by the drain timeout of tech_spec
     * §5.9.6, then ble_ota_flash_stop) is owned by exactly one task, decided by a CAS
     * (ble_ota_transport_disarm). The flash worker is never force-deleted while a
     * flash operation is draining (Phase 5 review CRITICAL-2 is preserved). */
    ble_ota_disarm_result_t r = ble_ota_transport_disarm();
    switch (r.kind) {
    case OTA_DISARM_OK:
        printf("fwupgrade: stopped\n");
        ESP_LOGI(TAG, "FWUPG_CMD cmd=stop outcome=ok");
        return 0;

    case OTA_DISARM_NOT_RUNNING:
        /* FR-906: no error escalation, no crash. */
        printf("fwupgrade: not running\n");
        ESP_LOGI(TAG, "FWUPG_CMD cmd=stop outcome=not_running");
        return 0;

    case OTA_DISARM_BUSY:
        printf("fwupgrade: a start or stop is in progress\n");
        ESP_LOGW(TAG, "FWUPG_CMD cmd=stop outcome=busy");
        return 1;

    case OTA_DISARM_IN_PROGRESS:
        printf("fwupgrade: a stop is already in progress (reason=%s)\n", r.reason);
        ESP_LOGW(TAG, "FWUPG_CMD cmd=stop outcome=busy");
        return 1;

    case OTA_DISARM_HOST_TIMEOUT:
        printf("fwupgrade: BLE host did not respond; OTA is still armed\n");
        ESP_LOGW(TAG, "FWUPG_CMD cmd=stop outcome=host_timeout");
        return 1;

    case OTA_DISARM_DRAIN_STUCK:
    default:
        printf("fwupgrade: OTA disarmed, but a flash operation is still draining -- "
               "the worker task was left running; %s (see FWUPG_DRAIN_TIMEOUT in the log)\n",
               DRAIN_STUCK_HINT);
        ESP_LOGW(TAG, "FWUPG_CMD cmd=stop outcome=drain_stuck");
        return 1;
    }
}

static int do_status(void)
{
    ESP_LOGI(TAG, "FWUPG_CMD cmd=status state=invoked");
    /* Read only: atomics and the existing protocol getters; never ble_host_exec.
     * "ble:", "advertising:", "connected:" and "bonded:" now describe the shared
     * host and link (documented change, REQ-INT-04). */
    ble_ota_transport_status_t st = { 0 };
    (void)ble_ota_transport_get_status(&st);
    printf("fwupgrade status:\n");
    printf("  ble: %s\n", st.host_up ? "up" : (st.host_starting ? "starting" : "down"));

    const char *armed_txt = "no";
    switch (st.arm_state) {
    case OTA_ARM_ARMED:       armed_txt = "yes"; break;
    case OTA_ARM_ARMING:      armed_txt = "arming"; break;
    case OTA_ARM_ABORTED:     /* shown as disarming */
    case OTA_ARM_DISARMING:   armed_txt = "disarming"; break;
    case OTA_ARM_DRAIN_STUCK: armed_txt = "drain_stuck"; break;
    default:                  armed_txt = "no"; break;
    }
    printf("  armed: %s\n", armed_txt);
    printf("  adv mode: %s\n", st.adv_mode_armed ? "armed" : "disarmed");
    printf("  advertising: %s\n", st.advertising ? "yes" : "no");
    printf("  connected: %s%s%s\n", st.connected ? "yes (" : "no", st.connected ? st.peer_addr_str : "",
           st.connected ? ")" : "");
    printf("  bonded: %s\n", st.bonded ? "yes" : "no");
    printf("  peer_connected: %s\n", st.connected ? "yes" : "no");
    printf("  cli_engaged: %s\n", st.cli_engaged ? "yes" : "no");
    printf("  ota_registered: %s\n", st.registered ? "yes" : "no");
    if (st.registered) {
        printf("  transfer state: %s\n", ble_ota_proto_state_name());
        uint32_t recv = 0, declared = 0;
        ble_ota_proto_get_progress(&recv, &declared);
        printf("  bytes received: %" PRIu32 "/%" PRIu32 "\n", recv, declared);
    }
    if (st.arm_state == OTA_ARM_DRAIN_STUCK || ble_ota_proto_is_drain_stuck()) {
        printf("  drain: stuck -- %s\n", DRAIN_STUCK_HINT);
    }
#if CONFIG_BLE_OTA_SUSPEND_WIFI
    /* tech_spec.md §6.7.5: no "auto-restore" framing -- the suspension,
     * once it happens, persists for the rest of the boot session. */
    if (s_wifi_suspended) {
        printf("  wifi: suspended (manual 'wifi_connect' required to restore)\n");
        printf("  wifi: pre-suspension ssid=%s\n", (const char *)s_wifi_saved_cfg.sta.ssid);
    } else if (s_wifi_suspend_failed) {
        printf("  wifi: suspend attempted and failed; Wi-Fi still active\n");
    } else {
        printf("  wifi: not suspended\n");
    }
#endif

    const char *label = NULL;
    size_t slot_size = 0;
    if (ble_ota_flash_get_target_slot(&label, &slot_size) == ESP_OK) {
        printf("  target partition: %s (%u bytes)\n", label ? label : "?", (unsigned)slot_size);
    }

    if (ble_ota_health_is_pending_verify()) {
        printf("  image state: PENDING_VERIFY (retry in <=%" PRIu32 " s)\n",
               ble_ota_health_remaining_deadline_s());
    } else {
        printf("  image state: validated\n");
    }
    ESP_LOGI(TAG, "FWUPG_CMD cmd=status outcome=ok");
    return 0;
}

static int cmd_fwupgrade_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_fwupgrade_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_fwupgrade_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_fwupgrade_args.subcmd->sval[0];
    if (strcmp(sub, "start") == 0) {
        return do_start();
    }
    if (strcmp(sub, "stop") == 0) {
        return do_stop();
    }
    if (strcmp(sub, "status") == 0) {
        return do_status();
    }

    /* FR-909: unknown subcommand produces a usage message, not UB. */
    printf("fwupgrade: unknown subcommand '%s' (usage: fwupgrade <start|stop|status>)\n", sub);
    return 1;
}

esp_err_t ble_ota_cmd_fwupgrade_register(void)
{
    /* tech_spec.md §6.7.1: registered unconditionally (not just when
     * CONFIG_BLE_OTA_SUSPEND_WIFI=y) so L2 always has a hook to call at
     * transfer-begin -- wifi_suspend_hook() itself is the thing that
     * differs by Kconfig (suspend vs. log-and-skip), not whether it is
     * wired up. */
    ble_ota_proto_set_wifi_suspend_cb(wifi_suspend_hook);

    s_fwupgrade_args.subcmd = arg_str1(NULL, NULL, "<start|stop|status>", "subcommand");
    s_fwupgrade_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "fwupgrade",
        .help = "Control the BLE OTA firmware update service (start|stop|status)",
        .hint = NULL,
        .func = &cmd_fwupgrade_handler,
        .argtable = &s_fwupgrade_args,
    };
    return esp_console_cmd_register(&cmd);
}
