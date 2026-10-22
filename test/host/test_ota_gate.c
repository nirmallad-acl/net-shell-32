/* TC-UT-06 / TC-UT-12: OTA arm state machine, RX/TX gates, on_connect-once and the
 * reboot guard slots (REQ-FUNC-31, 32, 75, 77, 79; tech_spec.md 2.4.3 and 2.4.4).
 *
 * LIMIT: these tests verify the DECISION LOGIC with interleavings scripted against
 * mock protocol state. The real atomicity (ble_host_exec on the NimBLE host task
 * versus a concurrent OTA RX write) depends on NimBLE's single-task event model and
 * is exercised only on target by E2E R-OTA-16. */
#include <string.h>

#include "unity.h"
#include "ble_ota.h"
#include "ble_ota_gate.h"

void setUp(void)
{
    ble_ota_gate_reset();
    ble_ota_gate_reset_guard();
}

void tearDown(void) {}

/* ================= arm state machine ================= */

static void arm_fully(void)
{
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_arm_commit());
}

void test_arm_sequence_and_exclusivity(void)
{
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DISARMED, ble_ota_gate_state());
    uint32_t e0 = ble_ota_gate_epoch();
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ARMING, ble_ota_gate_state());
    TEST_ASSERT_EQUAL_UINT32(e0 + 1, ble_ota_gate_epoch()); /* bumped right after the claim (R3-04) */
    TEST_ASSERT_FALSE(ble_ota_gate_try_arming());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_arm_commit());
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ARMED, ble_ota_gate_state());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_NOT_CLAIMED, ble_ota_gate_arm_commit()); /* already armed: no side effect */
    TEST_ASSERT_FALSE(ble_ota_gate_try_arming());
}

/* UT-G3(a): start's CAS ARMING -> DISARMED first, then the late arm_fn. */
void test_late_arm_after_cancel_has_no_effect(void)
{
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    TEST_ASSERT_TRUE(ble_ota_gate_cancel_arming());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_NOT_CLAIMED, ble_ota_gate_arm_commit());
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DISARMED, ble_ota_gate_state());
    /* (b) late arm_fn first: the cancel CAS then fails and the state is ARMED */
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_arm_commit());
    TEST_ASSERT_FALSE(ble_ota_gate_cancel_arming());
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ARMED, ble_ota_gate_state());
}

void test_rx_gate(void)
{
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_NOT_ARMED, ble_ota_gate_rx_decision());
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_NOT_ARMED, ble_ota_gate_rx_decision()); /* ARMING */
    ble_ota_gate_arm_commit();
    TEST_ASSERT_EQUAL_INT(OTA_RX_FORWARD, ble_ota_gate_rx_decision());
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_NOT_ARMED, ble_ota_gate_rx_decision()); /* ABORTED */
}

/* UT-G1: stop and expiry both request a disarm while ARMED. */
void test_concurrent_disarm_exactly_one_claims(void)
{
    arm_fully();
    uint32_t e = ble_ota_gate_epoch();
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_disarm_commit("fwupgrade_stop", 0, true, 4, 1000));
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ABORTED, ble_ota_gate_state());
    TEST_ASSERT_EQUAL_STRING("fwupgrade_stop", ble_ota_gate_disarm_reason());
    TEST_ASSERT_TRUE(ble_ota_gate_epoch() > e);
    /* the second request finds the state already left ARMED: nothing written */
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_NOT_CLAIMED,
                          ble_ota_gate_disarm_commit("readvertise_window_expired", 4, false, 4, 1100));
    TEST_ASSERT_EQUAL_STRING("fwupgrade_stop", ble_ota_gate_disarm_reason());
    /* exactly one drain claimant */
    TEST_ASSERT_TRUE(ble_ota_gate_claim_drain(OTA_ARM_ABORTED));
    TEST_ASSERT_FALSE(ble_ota_gate_claim_drain(OTA_ARM_ABORTED));
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DISARMING, ble_ota_gate_state());
    ble_ota_gate_drain_done(true);
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DISARMED, ble_ota_gate_state());
}

/* UT-G2: arm, disconnect (window epoch E), reconnect (E+1), then the stale expiry runs. */
void test_stale_epoch_expiry_is_a_no_op(void)
{
    arm_fully();
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_STALE, ble_ota_gate_disarm_commit("readvertise_window_expired", 5, false, 6, 1000));
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ARMED, ble_ota_gate_state());
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_STALE, ble_ota_gate_disarm_commit("readvertise_window_expired", 5, true, 5, 1000));
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ARMED, ble_ota_gate_state()); /* a link exists: stale */
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_disarm_commit("readvertise_window_expired", 5, false, 5, 1000));
    TEST_ASSERT_EQUAL_STRING("readvertise_window_expired", ble_ota_gate_disarm_reason());
}

/* UT-G4 / UT-G9: ABORTED janitor, timestamp never 0, claimant clears it. */
void test_aborted_janitor_rules(void)
{
    arm_fully();
    TEST_ASSERT_FALSE(ble_ota_gate_janitor_aborted_due(5000)); /* not ABORTED yet */
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, true, 1, 0); /* a zero clock still stores a non-zero stamp */
    TEST_ASSERT_FALSE(ble_ota_gate_janitor_aborted_due(200));    /* +0.2 s: disarm_fn still working */
    TEST_ASSERT_FALSE(ble_ota_gate_janitor_aborted_due(1000));   /* exactly 1 s is not older than 1 s */
    TEST_ASSERT_TRUE(ble_ota_gate_janitor_aborted_due(1100));    /* +1.1 s: claim */
    ble_ota_gate_clear_aborted_at();                             /* timestamp 0 = not set */
    TEST_ASSERT_FALSE(ble_ota_gate_janitor_aborted_due(60000));  /* never claimed with 0 */
    TEST_ASSERT_TRUE(ble_ota_gate_claim_drain(OTA_ARM_ABORTED)); /* but stop can still claim */
}

void test_aborted_fields_set_before_publication(void)
{
    arm_fully();
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 123456);
    /* Whoever sees ABORTED sees the original reason and a due timestamp. */
    TEST_ASSERT_EQUAL_INT(OTA_ARM_ABORTED, ble_ota_gate_state());
    TEST_ASSERT_EQUAL_STRING("fwupgrade_stop", ble_ota_gate_disarm_reason());
    TEST_ASSERT_TRUE(ble_ota_gate_janitor_aborted_due(123456 + 1100));
}

/* UT-G11 and the never-IDLE mock: DRAIN_STUCK blocks start and recovers. */
void test_drain_stuck_blocks_start_and_recovers(void)
{
    arm_fully();
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    TEST_ASSERT_TRUE(ble_ota_gate_claim_drain(OTA_ARM_ABORTED));
    ble_ota_gate_drain_done(false);
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DRAIN_STUCK, ble_ota_gate_state());
    TEST_ASSERT_FALSE(ble_ota_gate_try_arming()); /* start is refused */
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_NOT_ARMED, ble_ota_gate_rx_decision());
    TEST_ASSERT_FALSE(ble_ota_gate_claim_drain(OTA_ARM_ABORTED));
    TEST_ASSERT_TRUE(ble_ota_gate_claim_drain(OTA_ARM_DRAIN_STUCK)); /* janitor retry or stop/start */
    TEST_ASSERT_FALSE(ble_ota_gate_claim_drain(OTA_ARM_DRAIN_STUCK)); /* single claimant */
    ble_ota_gate_drain_done(true);
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DISARMED, ble_ota_gate_state());
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
}

void test_link_loss_policy_per_state(void)
{
    TEST_ASSERT_FALSE(ble_ota_gate_link_loss_needs_abort()); /* DISARMED */
    ble_ota_gate_try_arming();
    TEST_ASSERT_TRUE(ble_ota_gate_link_loss_needs_abort());  /* ARMING */
    ble_ota_gate_arm_commit();
    TEST_ASSERT_TRUE(ble_ota_gate_link_loss_needs_abort());  /* ARMED */
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    TEST_ASSERT_TRUE(ble_ota_gate_link_loss_needs_abort());  /* ABORTED */
    ble_ota_gate_claim_drain(OTA_ARM_ABORTED);
    TEST_ASSERT_TRUE(ble_ota_gate_link_loss_needs_abort());  /* DISARMING */
    ble_ota_gate_drain_done(false);
    TEST_ASSERT_TRUE(ble_ota_gate_link_loss_needs_abort());  /* DRAIN_STUCK */
    ble_ota_gate_claim_drain(OTA_ARM_DRAIN_STUCK);
    ble_ota_gate_drain_done(true);
    TEST_ASSERT_FALSE(ble_ota_gate_link_loss_needs_abort()); /* DISARMED again */
}

/* on_connect exactly once per generation, and only when it can be delivered. */
void test_on_connect_once_per_generation(void)
{
    TEST_ASSERT_FALSE(ble_ota_gate_on_connect_due(true, true, 5)); /* disarmed: never */
    arm_fully();
    TEST_ASSERT_FALSE(ble_ota_gate_on_connect_due(false, true, 5)); /* not secure yet */
    TEST_ASSERT_FALSE(ble_ota_gate_on_connect_due(true, false, 5)); /* not subscribed yet */
    TEST_ASSERT_TRUE(ble_ota_gate_on_connect_due(true, true, 5));
    TEST_ASSERT_FALSE(ble_ota_gate_on_connect_due(true, true, 5)); /* ENC_CHANGE / RESTORE again: no repeat */
    TEST_ASSERT_TRUE(ble_ota_gate_on_connect_due(true, true, 6));  /* new generation */
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    ble_ota_gate_claim_drain(OTA_ARM_ABORTED);
    ble_ota_gate_drain_done(true);
    arm_fully();
    TEST_ASSERT_TRUE(ble_ota_gate_on_connect_due(true, true, 6)); /* re-arm gives exactly one more */
    TEST_ASSERT_FALSE(ble_ota_gate_on_connect_due(true, true, 6));
}

/* ================= TX gate ================= */

static ota_tx_in_t good_tx(void)
{
    ota_tx_in_t in = { .item_gen = 3, .item_epoch = 0, .link_present = true, .cur_gen = 3, .subscribed = true,
                       .secure = true };
    in.item_epoch = ble_ota_gate_epoch();
    return in;
}

void test_tx_gate_conditions(void)
{
    ota_tx_in_t in = good_tx();
    TEST_ASSERT_EQUAL_INT(OTA_TX_NOT_ARMED, ble_ota_gate_tx_check(&in));
    arm_fully();
    in = good_tx();
    TEST_ASSERT_EQUAL_INT(OTA_TX_OK, ble_ota_gate_tx_check(&in));
    in = good_tx();
    in.link_present = false;
    TEST_ASSERT_EQUAL_INT(OTA_TX_NO_LINK, ble_ota_gate_tx_check(&in));
    in = good_tx();
    in.cur_gen = 4;
    TEST_ASSERT_EQUAL_INT(OTA_TX_STALE_GEN, ble_ota_gate_tx_check(&in));
    in = good_tx();
    in.subscribed = false;
    TEST_ASSERT_EQUAL_INT(OTA_TX_NOT_SUBSCRIBED, ble_ota_gate_tx_check(&in));
    in = good_tx();
    in.secure = false;
    TEST_ASSERT_EQUAL_INT(OTA_TX_NOT_SECURE, ble_ota_gate_tx_check(&in));
    TEST_ASSERT_EQUAL_STRING("not_secure", ble_ota_gate_tx_reason(OTA_TX_NOT_SECURE));
    TEST_ASSERT_EQUAL_STRING("stale_epoch", ble_ota_gate_tx_reason(OTA_TX_STALE_EPOCH));
}

/* UT-G7 / UT-G10: a frame from an earlier arm period is never delivered after a fast
 * stop -> start on the same generation, and no instant has ARMED with the old epoch. */
void test_arm_epoch_drops_old_frames(void)
{
    arm_fully();
    ota_tx_in_t old = good_tx(); /* e.g. the "ERR aborted" frame, enqueued with epoch E */
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    ble_ota_gate_claim_drain(OTA_ARM_ABORTED);
    ble_ota_gate_drain_done(true);

    TEST_ASSERT_TRUE(ble_ota_gate_try_arming()); /* epoch bumped while ARMING */
    TEST_ASSERT_EQUAL_INT(OTA_TX_NOT_ARMED, ble_ota_gate_tx_check(&old)); /* gate drops everything in ARMING */
    TEST_ASSERT_EQUAL_INT(OTA_EXEC_CLAIMED, ble_ota_gate_arm_commit());
    /* ARMED is now visible, and the old frame's epoch is already stale */
    TEST_ASSERT_EQUAL_INT(OTA_TX_STALE_EPOCH, ble_ota_gate_tx_check(&old));
    ota_tx_in_t fresh = good_tx();
    TEST_ASSERT_EQUAL_INT(OTA_TX_OK, ble_ota_gate_tx_check(&fresh));
}

/* ================= reboot guard ================= */

static bool g_idle, g_stuck, g_pending, g_gen_ok;
static uintptr_t g_last_arg;
static int g_exec_calls;
typedef esp_err_t (*exec_beh_t)(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res);
static exec_beh_t g_beh;

static esp_err_t beh_run(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    *res = fn(arg);
    return ESP_OK;
}

static esp_err_t beh_indeterminate_not_run(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)fn;
    (void)arg;
    (void)res;
    return ESP_ERR_BLE_HOST_EXEC_INDETERMINATE;
}

static esp_err_t beh_indeterminate_after_run(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)res;
    (void)fn(arg); /* the guard ran and committed, but the caller gives up waiting */
    return ESP_ERR_BLE_HOST_EXEC_INDETERMINATE;
}

static esp_err_t beh_timeout_not_run(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)fn;
    (void)arg;
    (void)res;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t beh_not_up(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)fn;
    (void)arg;
    (void)res;
    return ESP_ERR_BLE_HOST_NOT_UP;
}

static esp_err_t beh_wrong_task(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)fn;
    (void)arg;
    (void)res;
    return ESP_ERR_BLE_HOST_WRONG_TASK;
}

static esp_err_t mock_exec(ble_host_exec_fn_t fn, uintptr_t arg, uint32_t timeout_ms, int32_t *res)
{
    (void)timeout_ms;
    g_exec_calls++;
    g_last_arg = arg;
    return (g_beh != NULL ? g_beh : beh_run)(fn, arg, res);
}

static bool op_gen(uint32_t gen) { (void)gen; return g_gen_ok; }
static bool op_idle(void) { return g_idle; }
static bool op_stuck(void) { return g_stuck; }
static bool op_pending(void) { return g_pending; }

static const ble_ota_guard_ops_t ops = {
    .exec = mock_exec, .gen_current_secure = op_gen, .proto_idle = op_idle, .proto_drain_stuck = op_stuck,
    .pending_verify = op_pending,
};

static void guard_setup(void)
{
    g_idle = true;
    g_stuck = false;
    g_pending = false;
    g_gen_ok = true;
    g_beh = NULL;
    g_exec_calls = 0;
    ble_ota_gate_set_guard_ops(&ops);
}

void test_guard_commits_when_idle_and_rejects_ota_rx_afterwards(void)
{
    guard_setup();
    bool ovr = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, &ovr));
    TEST_ASSERT_FALSE(ovr);
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_REBOOT_PENDING, ble_ota_gate_rx_decision()); /* nothing leaves IDLE now */
}

void test_guard_refuses_each_non_idle_state(void)
{
    guard_setup();
    g_idle = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE, ble_ota_gate_guard_commit(5, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending()); /* armed-IDLE or session never refuse; only a non-IDLE protocol */
}

void test_guard_pending_verify_and_stale_generation(void)
{
    guard_setup();
    g_pending = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY, ble_ota_gate_guard_commit(0, 1000, NULL));
    g_pending = false;
    g_gen_ok = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_STALE, ble_ota_gate_guard_commit(7, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL)); /* serial: no link precheck */
}

/* UT-R1: a late guard_fn after the caller cancelled has no effect at all. */
void test_ut_r1_late_guard_after_caller_cancel(void)
{
    guard_setup();
    g_beh = beh_indeterminate_not_run;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, ble_ota_gate_guard_commit(0, 1000, NULL)); /* ERR internal path */
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_CANCELLED, ble_ota_gate_guard_fn(g_last_arg)); /* every check would pass */
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    /* a following OTA RX is accepted while armed */
    TEST_ASSERT_TRUE(ble_ota_gate_try_arming());
    ble_ota_gate_arm_commit();
    TEST_ASSERT_EQUAL_INT(OTA_RX_FORWARD, ble_ota_gate_rx_decision());
    /* same with exec TIMEOUT and a (contract-violating) late run */
    ble_ota_gate_reset_guard();
    g_beh = beh_timeout_not_run;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_CANCELLED, ble_ota_gate_guard_fn(g_last_arg));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
}

/* UT-R2: guard committed but the caller gave up waiting: the request IS committed. */
void test_ut_r2_guard_then_cancel_ends_in_restart(void)
{
    guard_setup();
    g_beh = beh_indeterminate_after_run;
    bool ovr = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, &ovr)); /* the caller must restart */
    TEST_ASSERT_FALSE(ovr);
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());
    TEST_ASSERT_EQUAL_INT(OTA_RX_REJECT_REBOOT_PENDING, ble_ota_gate_rx_decision());
    /* variant: rc OK with res COMMITTED */
    ble_ota_gate_reset_guard();
    g_beh = beh_run;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());
}

static esp_err_t beh_old_guard_runs_late_then_new(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res);
static uintptr_t g_old_arg;
static int32_t g_old_result = -1;

static esp_err_t beh_record_arg_and_timeout(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    (void)fn;
    (void)res;
    g_old_arg = arg;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t beh_old_guard_runs_late_then_new(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    g_old_result = fn(g_old_arg); /* a late guard_fn for ticket t runs after ticket t+1 was opened */
    *res = fn(arg);
    return ESP_OK;
}

/* UT-R3: ticket reuse. */
void test_ut_r3_late_guard_for_old_ticket_cannot_touch_the_new_slot(void)
{
    guard_setup();
    g_beh = beh_record_arg_and_timeout;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, ble_ota_gate_guard_commit(0, 1000, NULL));
    g_beh = beh_old_guard_runs_late_then_new;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL)); /* guard_fn(t+1) decides on its own */
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_CANCELLED, g_old_result);
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());
}

static esp_err_t beh_reenter_same_slot(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    /* a second open request of the same transport while the first is still open */
    g_beh = beh_run;
    esp_err_t inner = ble_ota_gate_guard_commit(0, 1000, NULL);
    g_old_result = inner;
    *res = fn(arg);
    return ESP_OK;
}

void test_ut_r3_second_open_on_the_same_slot_is_invalid_state(void)
{
    guard_setup();
    g_beh = beh_reenter_same_slot;
    g_old_result = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, g_old_result);
}

static esp_err_t beh_nested_ble(ble_host_exec_fn_t fn, uintptr_t arg, int32_t *res)
{
    g_beh = beh_run;
    g_old_result = ble_ota_gate_guard_commit(9, 1000, NULL); /* a BLE request while the serial one is in flight */
    *res = fn(arg);
    return ESP_OK;
}

/* UT-R4: the two transports are independent. */
void test_ut_r4_transports_are_independent(void)
{
    guard_setup();
    g_beh = beh_nested_ble;
    g_old_result = -1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g_old_result); /* both committed: neither reports a refusal */
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());
}

/* UT-R5: NOT_UP direct path, commit-then-check. */
void test_ut_r5_direct_path(void)
{
    guard_setup();
    g_beh = beh_not_up;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending());

    ble_ota_gate_reset_guard();
    g_idle = false; /* a refusal reverts to CANCELLED */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    g_idle = true;
    g_pending = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    g_pending = false;
    /* a BLE request cannot hold a current link before the host is up */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_STALE, ble_ota_gate_guard_commit(3, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
}

/* UT-R6: G2-07, a stuck drain does not block reboot (but PENDING_VERIFY still does). */
void test_ut_r6_drain_stuck_exception(void)
{
    guard_setup();
    g_idle = false; /* ABORTING_DRAIN, not stuck */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE, ble_ota_gate_guard_commit(0, 1000, NULL));

    g_stuck = true; /* ble_ota_proto_is_drain_stuck() */
    bool ovr = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, &ovr));
    TEST_ASSERT_TRUE(ovr);
    TEST_ASSERT_TRUE(ble_ota_gate_reboot_pending()); /* the COMMITTED_OVR encoding counts as committed */

    ble_ota_gate_reset_guard();
    g_pending = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_PENDING_VERIFY, ble_ota_gate_guard_commit(0, 1000, NULL));
    g_pending = false;
    g_stuck = false; /* is_drain_stuck() mutex timeout returns false: conservative OTA_ACTIVE */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_OTA_REBOOT_OTA_ACTIVE, ble_ota_gate_guard_commit(0, 1000, NULL));

    /* arm_state == DRAIN_STUCK is enough on its own (no protocol mutex needed) */
    arm_fully();
    ble_ota_gate_disarm_commit("fwupgrade_stop", 0, false, 1, 1000);
    ble_ota_gate_claim_drain(OTA_ARM_ABORTED);
    ble_ota_gate_drain_done(false);
    TEST_ASSERT_EQUAL_INT(OTA_ARM_DRAIN_STUCK, ble_ota_gate_state());
    ovr = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, &ovr));
    TEST_ASSERT_TRUE(ovr);
}

void test_guard_exec_codes_pass_through(void)
{
    guard_setup();
    g_beh = beh_wrong_task;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_HOST_WRONG_TASK, ble_ota_gate_guard_commit(0, 1000, NULL));
    TEST_ASSERT_FALSE(ble_ota_gate_reboot_pending());
    /* the slot was closed: the next request is accepted */
    g_beh = beh_run;
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_ota_gate_guard_commit(0, 1000, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arm_sequence_and_exclusivity);
    RUN_TEST(test_late_arm_after_cancel_has_no_effect);
    RUN_TEST(test_rx_gate);
    RUN_TEST(test_concurrent_disarm_exactly_one_claims);
    RUN_TEST(test_stale_epoch_expiry_is_a_no_op);
    RUN_TEST(test_aborted_janitor_rules);
    RUN_TEST(test_aborted_fields_set_before_publication);
    RUN_TEST(test_drain_stuck_blocks_start_and_recovers);
    RUN_TEST(test_link_loss_policy_per_state);
    RUN_TEST(test_on_connect_once_per_generation);
    RUN_TEST(test_tx_gate_conditions);
    RUN_TEST(test_arm_epoch_drops_old_frames);
    RUN_TEST(test_guard_commits_when_idle_and_rejects_ota_rx_afterwards);
    RUN_TEST(test_guard_refuses_each_non_idle_state);
    RUN_TEST(test_guard_pending_verify_and_stale_generation);
    RUN_TEST(test_ut_r1_late_guard_after_caller_cancel);
    RUN_TEST(test_ut_r2_guard_then_cancel_ends_in_restart);
    RUN_TEST(test_ut_r3_late_guard_for_old_ticket_cannot_touch_the_new_slot);
    RUN_TEST(test_ut_r3_second_open_on_the_same_slot_is_invalid_state);
    RUN_TEST(test_ut_r4_transports_are_independent);
    RUN_TEST(test_ut_r5_direct_path);
    RUN_TEST(test_ut_r6_drain_stuck_exception);
    RUN_TEST(test_guard_exec_codes_pass_through);
    return UNITY_END();
}
