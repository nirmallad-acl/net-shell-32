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
 * FR-901..909: registered on the existing shared esp_console REPL. Owns
 * the "fwupgrade start" ordered bring-up / unwind (FR-102).
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

static int do_start(void)
{
    /* tech_spec.md §10.4(2): console commands are commands too -- each
     * logs its invocation and its outcome. */
    ESP_LOGI(TAG, "FWUPG_CMD cmd=start state=invoked");

    if (ble_ota_transport_is_running()) {
        /* FR-905: explicit "already running" error, never a double-init. */
        printf("fwupgrade: already running\n");
        ESP_LOGW(TAG, "FWUPG_CMD cmd=start outcome=already_running");
        return 1;
    }

    if (ble_ota_health_is_pending_verify()) {
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

    esp_err_t err = ble_ota_flash_start();
    if (err != ESP_OK) {
        printf("fwupgrade: failed to start flash worker (%s)\n", esp_err_to_name(err));
        ESP_LOGE(TAG, "FWUPG_CMD cmd=start outcome=flash_start_failed err=%s", esp_err_to_name(err));
        return 1;
    }

    err = ble_ota_transport_start();
    if (err != ESP_OK) {
        /* FR-102: abort on first failure, unwind everything already done. */
        ble_ota_flash_stop();
        if (err == ESP_ERR_BLE_OTA_PENDING_VERIFY) {
            printf("fwupgrade: cannot start -- device is in post-update health verification\n");
        } else {
            printf("fwupgrade: BLE bring-up failed (%s)\n", esp_err_to_name(err));
        }
        ESP_LOGE(TAG, "FWUPG_CMD cmd=start outcome=ble_bringup_failed err=%s", esp_err_to_name(err));
        return 1;
    }

    printf("fwupgrade: started (advertising)\n");
    ESP_LOGI(TAG, "FWUPG_CMD cmd=start outcome=ok");
    return 0;
}

/* tech_spec.md §6.7.5 (binding): "fwupgrade stop" tears the BLE stack
 * down (FR-103) and nothing more -- it must NEVER restore Wi-Fi. This is
 * explicitly called out as the single most likely place for an
 * implementer or reviewer to "helpfully" add a restore call; doing so
 * re-creates code_review.md MAJOR-4 verbatim, since this handler runs on
 * the REPL task, independently of whichever task ran the suspend. Do not
 * add one here. */
static int do_stop(void)
{
    ESP_LOGI(TAG, "FWUPG_CMD cmd=stop state=invoked");

    if (!ble_ota_transport_is_running()) {
        /* FR-906: no error escalation, no crash. */
        printf("fwupgrade: not running\n");
        ESP_LOGI(TAG, "FWUPG_CMD cmd=stop outcome=not_running");
        return 0;
    }

    esp_err_t err = ble_ota_transport_stop();
    if (err != ESP_OK) {
        printf("fwupgrade: BLE teardown reported %s (continuing cleanup)\n", esp_err_to_name(err));
    }

    /* Phase 5 review CRITICAL-2 fix: ble_ota_transport_stop() above only
     * *requests* the L2/L3 abort (tech_spec.md §5.9) -- it does not wait
     * for the flash worker to actually release ownership of the
     * reassembly buffer / OTA handle. Force-deleting the worker task
     * (ble_ota_flash_stop()) before that handoff completes is exactly the
     * "buffer/task torn out from under an in-flight esp_ota_write()" race
     * §5.9's ABORTING_DRAIN state exists to prevent -- it must never
     * happen via this path either. Wait (bounded by the same drain
     * timeout §5.9.6 already defines) for the state machine to actually
     * reach IDLE before touching the worker task. */
    if (!ble_ota_proto_wait_idle(CONFIG_BLE_OTA_DRAIN_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "fwupgrade stop: drain did not complete within %d ms; "
                       "refusing to force-stop the flash worker task",
                 CONFIG_BLE_OTA_DRAIN_TIMEOUT_MS);
        printf("fwupgrade: BLE stopped, but a flash operation is still draining -- "
               "the worker task was left running; a device reset is required "
               "(see FWUPG_DRAIN_TIMEOUT in the log)\n");
        ESP_LOGE(TAG, "FWUPG_CMD cmd=stop outcome=drain_stuck");
        return 1;
    }

    ble_ota_flash_stop();
    printf("fwupgrade: stopped\n");
    ESP_LOGI(TAG, "FWUPG_CMD cmd=stop outcome=ok");
    return 0;
}

static int do_status(void)
{
    ESP_LOGI(TAG, "FWUPG_CMD cmd=status state=invoked");
    bool running = ble_ota_transport_is_running();
    printf("fwupgrade status:\n");
    printf("  ble: %s\n", running ? "up" : "down");

    if (running) {
        ble_ota_transport_status_t st = { 0 };
        if (ble_ota_transport_get_status(&st) == ESP_OK) {
            printf("  advertising: %s\n", st.advertising ? "yes" : "no");
            printf("  connected: %s%s%s\n", st.connected ? "yes (" : "no",
                   st.connected ? st.peer_addr_str : "", st.connected ? ")" : "");
            printf("  bonded: %s\n", st.bonded ? "yes" : "no");
        }
        printf("  transfer state: %s\n", ble_ota_proto_state_name());
        uint32_t recv = 0, declared = 0;
        ble_ota_proto_get_progress(&recv, &declared);
        printf("  bytes received: %" PRIu32 "/%" PRIu32 "\n", recv, declared);
        if (ble_ota_proto_is_drain_stuck()) {
            printf("  drain: stuck -- requires a reset (see FWUPG_DRAIN_TIMEOUT in the log)\n");
        }
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
