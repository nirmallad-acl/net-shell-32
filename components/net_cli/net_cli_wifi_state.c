/*
 * net_cli_wifi_state.c -- sole owner of this feature's cross-context state.
 *
 * Everything that is touched by more than one execution context lives here
 * and nowhere else (P-B4): the connect event group, the ping semaphore, the
 * last-disconnect-reason, the connect-armed flag, the ping-abandoned flag
 * and the boot-status record. This is also the only file that registers or
 * runs the WIFI_EVENT/IP_EVENT handler. See tech_spec.md §6.4, §9.2, §9.3.
 *
 * All FreeRTOS primitives are created with the *Static variants, so this
 * module's entire concurrency layer costs zero heap bytes (NFR-9) -- backing
 * storage lands in BSS, sized at compile time.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_err.h"
#include "net_cli.h"
#include "net_cli_internal.h"

#define NET_CLI_WIFI_CONNECTED_BIT  BIT0  /* IP_EVENT_STA_GOT_IP seen         */
#define NET_CLI_WIFI_FAIL_BIT       BIT1  /* WIFI_EVENT_STA_DISCONNECTED seen */

/* ---- boot-time, statically allocated cross-context state (§9.2) ---- */

static StaticEventGroup_t           s_wifi_evt_grp_storage;
static EventGroupHandle_t           s_wifi_evt_grp;
static esp_event_handler_instance_t s_wifi_evt_inst;
static esp_event_handler_instance_t s_ip_evt_inst;

static StaticSemaphore_t     s_ping_done_storage;
static SemaphoreHandle_t     s_ping_done_sem;
static net_cli_ping_state_t  s_ping_state;

/* Written by the event-loop task, read by the REPL task. Single-word,
 * volatile; publication is ordered by the event-group bit (§9.3): the
 * reason is stored BEFORE FAIL_BIT is set, and read only AFTER
 * xEventGroupWaitBits() observes FAIL_BIT. */
static volatile uint8_t s_last_disconnect_reason;

/* Written by the REPL task (arm/disarm around a connect attempt), read by
 * the event-loop task. Not a lock -- see §7.3.5 / §9.3. */
static volatile bool s_connect_armed;

static net_cli_boot_status_t s_boot_status;   /* written once, by value, at boot step 9 (DA-10) */
static bool                  s_state_initialized;

/* ---- Wi-Fi/IP event handlers -- deliberately minimal (NFR-19) ----
 * A byte store and a bit set, nothing else: no printf, no ESP_LOGx, no
 * blocking call, no allocation, no esp_wifi_ or esp_netif_ call. This keeps
 * the system event loop unblocked and guarantees our code contributes zero
 * log/print output from event context (NFR-4). */

static void wifi_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        if (d != NULL) {
            s_last_disconnect_reason = d->reason;   /* store BEFORE the bit */
        }
        if (s_connect_armed) {
            xEventGroupSetBits(s_wifi_evt_grp, NET_CLI_WIFI_FAIL_BIT);
        }
    }
}

static void ip_evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* Unconditional -- does NOT consult s_connect_armed. A lease that
         * arrives after a Timeout verdict still sets the bit; the NEXT
         * invocation's pre-connect clear is what absorbs it (§6.4). */
        xEventGroupSetBits(s_wifi_evt_grp, NET_CLI_WIFI_CONNECTED_BIT);
    }
}

/* ---- boot-time setup ---- */

esp_err_t net_cli_wifi_state_init(void)
{
    if (s_state_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_evt_grp = xEventGroupCreateStatic(&s_wifi_evt_grp_storage);
    if (s_wifi_evt_grp == NULL) {
        return ESP_ERR_NO_MEM;   /* static creation "cannot fail"; defensive only */
    }

    s_ping_done_sem = xSemaphoreCreateBinaryStatic(&s_ping_done_storage);
    if (s_ping_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_evt_handler, NULL,
                                                          &s_wifi_evt_inst);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_evt_handler, NULL,
                                               &s_ip_evt_inst);
    if (err != ESP_OK) {
        return err;
    }

    s_ping_state.abandoned = false;
    s_state_initialized = true;
    return ESP_OK;
}

esp_err_t net_cli_set_boot_status(const net_cli_boot_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_boot_status = *status;   /* by-value copy -- DA-10; never retain the pointer */
    return ESP_OK;
}

const net_cli_boot_status_t *net_cli_boot_status_get(void)
{
    return &s_boot_status;
}

bool net_cli_wifi_ready(void)
{
    return (s_boot_status.wifi_init == ESP_OK) &&
           (s_boot_status.wifi_set_mode == ESP_OK) &&
           (s_boot_status.wifi_start == ESP_OK) &&
           (s_boot_status.cli_init == ESP_OK);           /* DA-2 conjunct */
}

bool net_cli_sync_ready(void)
{
    return (s_boot_status.cli_init == ESP_OK) &&
           (s_wifi_evt_grp != NULL) &&
           (s_ping_done_sem != NULL);
}

SemaphoreHandle_t net_cli_ping_done_sem(void)
{
    return s_ping_done_sem;
}

net_cli_ping_state_t *net_cli_ping_state(void)
{
    return &s_ping_state;
}

/* ---- the §6.4 connect sequence, in full ---- */

esp_err_t net_cli_wifi_connect_attempt(const wifi_config_t *cfg,
                                        uint32_t timeout_ms,
                                        net_cli_connect_result_t *out)
{
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!net_cli_sync_ready()) {
        return ESP_ERR_INVALID_STATE;   /* DA-2 -- never block on a NULL handle */
    }

    /* Step 1: disarm before the pre-emptive disconnect, so its deauth event
     * (if any) is never counted as the outcome of THIS attempt. */
    s_connect_armed = false;

    /* Step 2: pre-emptive disconnect (GATE 1 / OQ-5). Return value captured
     * per NFR-15 / DA-11. ESP_ERR_WIFI_NOT_CONNECT / _NOT_STARTED mean
     * nothing was associated, which is the common case and not an error
     * worth surfacing; any other code is likewise non-fatal here -- the
     * connect attempt the operator asked for proceeds regardless. Not
     * surfaced to the console: §7.3.4's output contract is exactly two
     * lines, and command output must never carry ESP_LOGx output that could
     * land mid-command (§7.0 rule 1 / §8.6 / NFR-4). */
    esp_err_t disconnect_err = esp_wifi_disconnect();
    (void)disconnect_err;

    /* Step 3: settle window -- let any deauth event drain while disarmed.
     * Heuristic, not a guarantee; see tech_spec.md R-2. */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Steps 4/5: drop anything stale, reset the reason. */
    xEventGroupClearBits(s_wifi_evt_grp, NET_CLI_WIFI_CONNECTED_BIT | NET_CLI_WIFI_FAIL_BIT);
    s_last_disconnect_reason = 0;

    /* Step 6: arm for the real attempt. */
    s_connect_armed = true;

    /* Step 7: apply credentials and connect. */
    wifi_config_t local_cfg = *cfg;
    esp_err_t set_err = esp_wifi_set_config(WIFI_IF_STA, &local_cfg);
    if (set_err != ESP_OK) {
        s_connect_armed = false;
        return set_err;
    }

    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        s_connect_armed = false;
        return connect_err;
    }

    /* Step 8: bounded wait for either outcome bit (NFR-3 / OQ-5). */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt_grp,
                                            NET_CLI_WIFI_CONNECTED_BIT | NET_CLI_WIFI_FAIL_BIT,
                                            pdFALSE  /* do not clear on exit */,
                                            pdFALSE  /* wait for EITHER bit */,
                                            pdMS_TO_TICKS(timeout_ms));

    /* Step 9: non-blocking re-read. A GOT_IP that lands between the wait's
     * internal timeout expiry and this line is a real, completed connection
     * (DA-3) -- classification below must see it. */
    if ((bits & (NET_CLI_WIFI_CONNECTED_BIT | NET_CLI_WIFI_FAIL_BIT)) == 0) {
        bits |= xEventGroupGetBits(s_wifi_evt_grp);
    }

    /* Step 10: disarm. */
    s_connect_armed = false;

    /* Step 11: classify -- CONNECTED_BIT is tested first and wins outright. */
    if (bits & NET_CLI_WIFI_CONNECTED_BIT) {
        out->outcome = NET_CLI_CONNECT_CONNECTED;
    } else if (bits & NET_CLI_WIFI_FAIL_BIT) {
        out->outcome = NET_CLI_CONNECT_FAILED;
        out->reason  = s_last_disconnect_reason;
    } else {
        /* DA-3: no esp_wifi_disconnect() here. The association, if any, is
         * left exactly as the driver has it; the next invocation's step 2
         * cleans up any leftover association. */
        out->outcome = NET_CLI_CONNECT_TIMEOUT;
    }

    /* Step 12: leave the group clean for the next invocation. */
    xEventGroupClearBits(s_wifi_evt_grp, NET_CLI_WIFI_CONNECTED_BIT | NET_CLI_WIFI_FAIL_BIT);

    return ESP_OK;
}
