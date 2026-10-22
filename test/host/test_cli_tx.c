/* TC-UT-10 (TX part) and TC-UT-04 (security part): BLE CLI TX/RX gate decisions, the
 * TX-failure rule and the input-overflow latch (REQ-FUNC-48, 60, 61, 63; ADV-CONC-04,
 * ADV-IMPL-05). The FreeRTOS queue plumbing of ble_cli.c is NOT covered here (E2E). */
#include <string.h>

#include "unity.h"
#include "cli_line_asm.h"
#include "cli_tx_gate.h"

void setUp(void) {}
void tearDown(void) {}

void test_tx_decision_table(void)
{
    /* args: line_gen, cur_secure_gen, failed_gen, subscribed */
    TEST_ASSERT_EQUAL_INT(CLI_TX_SEND, cli_tx_decide(5, 5, 0, true));
    /* an unbonded / insecure peer: cur_secure_gen == 0 -> nothing is ever sent, even if subscribed */
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_STALE, cli_tx_decide(5, 0, 0, true));
    /* a line of an older generation is dropped */
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_STALE, cli_tx_decide(4, 5, 0, true));
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_STALE, cli_tx_decide(6, 5, 0, true));
    /* terminally failed generation */
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_FAILED, cli_tx_decide(5, 5, 5, true));
    TEST_ASSERT_EQUAL_INT(CLI_TX_SEND, cli_tx_decide(5, 5, 4, true)); /* a failure of another generation does not matter */
    /* the CCCD must be enabled: an inert subscription sends nothing */
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_UNSUBSCRIBED, cli_tx_decide(5, 5, 0, false));
    /* precedence: stale before failed before unsubscribed */
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_STALE, cli_tx_decide(5, 0, 5, false));
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_FAILED, cli_tx_decide(5, 5, 5, false));
}

void test_sink_admission(void)
{
    TEST_ASSERT_TRUE(cli_sink_admit(5, 5, 0));
    TEST_ASSERT_FALSE(cli_sink_admit(5, 0, 0)); /* not secure */
    TEST_ASSERT_FALSE(cli_sink_admit(4, 5, 0)); /* stale generation: a session finds out from its own write */
    TEST_ASSERT_FALSE(cli_sink_admit(5, 5, 5)); /* tx_failed */
    TEST_ASSERT_TRUE(cli_sink_admit(5, 5, 4));
    TEST_ASSERT_FALSE(cli_sink_admit(0, 0, 0));
}

/* ADV-IMPL-05: only "link gone / no longer secure" is terminal. */
void test_only_a_gone_link_is_a_terminal_tx_failure(void)
{
    TEST_ASSERT_TRUE(cli_tx_failure_is_terminal(ESP_ERR_INVALID_STATE));
    TEST_ASSERT_FALSE(cli_tx_failure_is_terminal(ESP_ERR_NO_MEM)); /* congestion: retry, not terminal */
    TEST_ASSERT_FALSE(cli_tx_failure_is_terminal(ESP_FAIL));
    TEST_ASSERT_FALSE(cli_tx_failure_is_terminal(ESP_ERR_TIMEOUT));
    TEST_ASSERT_FALSE(cli_tx_failure_is_terminal(ESP_OK));
}

/* ADV-R2-01: a transient failure after some slices were sent leaves a truncated line; one
 * resync LF is sent before the next line of the same generation, and only then. */
void test_resync_lf_after_a_mid_line_transient_failure(void)
{
    cli_resync_t r;
    cli_resync_init(&r);
    TEST_ASSERT_FALSE(cli_resync_take(&r, 5)); /* nothing pending */

    cli_resync_note_failure(&r, 5, 2, ESP_ERR_NO_MEM); /* 2 slices went out, the 3rd failed */
    TEST_ASSERT_TRUE(cli_resync_take(&r, 5));
    TEST_ASSERT_FALSE(cli_resync_take(&r, 5)); /* exactly once */

    cli_resync_note_failure(&r, 5, 0, ESP_ERR_NO_MEM); /* nothing of the line was sent: no fragment */
    TEST_ASSERT_FALSE(cli_resync_take(&r, 5));

    cli_resync_note_failure(&r, 5, 3, ESP_ERR_INVALID_STATE); /* terminal: the link is gone */
    TEST_ASSERT_FALSE(cli_resync_take(&r, 5));

    cli_resync_note_failure(&r, 5, 1, ESP_FAIL);
    TEST_ASSERT_FALSE(cli_resync_take(&r, 6)); /* another generation: discarded, never sent */
    TEST_ASSERT_FALSE(cli_resync_take(&r, 5));
}

void test_rx_security_verdicts(void)
{
    TEST_ASSERT_EQUAL_INT(CLI_SEC_NEED_ENCRYPTION, cli_rx_sec_verdict(false, false));
    TEST_ASSERT_EQUAL_INT(CLI_SEC_NEED_ENCRYPTION, cli_rx_sec_verdict(false, true));
    TEST_ASSERT_EQUAL_INT(CLI_SEC_NEED_BOND, cli_rx_sec_verdict(true, false)); /* encrypted but not bonded */
    TEST_ASSERT_EQUAL_INT(CLI_SEC_OK, cli_rx_sec_verdict(true, true));
}

/* ---- input-overflow latch with a mock command queue ---- */

typedef struct {
    int items[16];
    int n;
    int depth;
} mockq_t;

static mockq_t q;
static cli_latch_t latch;
static int g_queued_reboot;

static bool push(void *ctx)
{
    const int *item = (const int *)ctx;
    if (q.n >= q.depth) {
        return false;
    }
    q.items[q.n++] = *item;
    if (*item == 4) {
        g_queued_reboot++;
    }
    return true;
}

static int g_full_events;
static void on_result(const cli_result_t *r, void *user)
{
    uint32_t gen = *(const uint32_t *)user;
    int item = r->cmd_id;
    if (cli_rx_offer(&latch, gen, push, &item) == CLI_RX_DROPPED_FULL) {
        g_full_events++;
    }
}

static int lookup(const char *name, size_t len)
{
    static const char *const names[5] = { "hw_version", "fw_version", "accel_xyz", "reboot_reason", "reboot" };
    for (int i = 0; i < 5; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], name, len) == 0) {
            return i;
        }
    }
    return -1;
}

static int pop(void)
{
    int v = q.items[0];
    memmove(q.items, &q.items[1], (size_t)(q.n - 1) * sizeof(int));
    q.n--;
    return v;
}

/* 6 x hw_version then reboot in ONE write with a queue depth of 4: 4 responses, one
 * ERR busy, no reboot (the "later command never overtakes a dropped one" rule). */
void test_overflow_latch_burst_of_six_plus_reboot(void)
{
    memset(&q, 0, sizeof(q));
    q.depth = 4;
    g_queued_reboot = 0;
    g_full_events = 0;
    cli_latch_init(&latch);
    cli_line_asm_t a;
    cli_line_asm_reset(&a, 7);
    uint32_t gen = 7;
    const char *burst = "hw_version\nhw_version\nhw_version\nhw_version\nhw_version\nhw_version\nreboot\n";
    TEST_ASSERT_EQUAL_UINT32(7, cli_line_asm_feed(&a, (const uint8_t *)burst, strlen(burst), lookup, on_result, &gen));

    TEST_ASSERT_EQUAL_INT(4, q.n);
    TEST_ASSERT_EQUAL_INT(1, g_full_events); /* exactly one "queue_full" episode */
    TEST_ASSERT_EQUAL_INT(0, g_queued_reboot); /* reboot was dropped, never queued */
    TEST_ASSERT_TRUE(cli_latch_active(&latch));

    uint32_t g_out = 0;
    TEST_ASSERT_FALSE(cli_latch_ready_to_clear(&latch, false, &g_out)); /* queue not drained yet */
    int ran = 0;
    while (q.n > 0) {
        TEST_ASSERT_EQUAL_INT(0, pop()); /* all four are hw_version, in order */
        ran++;
    }
    TEST_ASSERT_EQUAL_INT(4, ran);

    /* worker: queue empty and latch set -> exactly one ERR busy, then clear */
    int busy_lines = 0;
    if (cli_latch_ready_to_clear(&latch, true, &g_out)) {
        busy_lines++;
        TEST_ASSERT_EQUAL_UINT32(7, g_out);
        cli_latch_clear(&latch);
    }
    TEST_ASSERT_EQUAL_INT(1, busy_lines);
    TEST_ASSERT_FALSE(cli_latch_ready_to_clear(&latch, true, &g_out));

    /* afterwards new input is accepted again */
    const char *next = "fw_version\n";
    cli_line_asm_feed(&a, (const uint8_t *)next, strlen(next), lookup, on_result, &gen);
    TEST_ASSERT_EQUAL_INT(1, q.n);
    TEST_ASSERT_EQUAL_INT(1, pop());
}

/* While the latch is set, even a command that would fit (room freed meanwhile) is
 * dropped: order is never violated. */
void test_latched_items_are_dropped_even_with_room(void)
{
    memset(&q, 0, sizeof(q));
    q.depth = 1;
    g_full_events = 0;
    cli_latch_init(&latch);
    int item = 0;
    TEST_ASSERT_EQUAL_INT(CLI_RX_QUEUED, cli_rx_offer(&latch, 3, push, &item));
    TEST_ASSERT_EQUAL_INT(CLI_RX_DROPPED_FULL, cli_rx_offer(&latch, 3, push, &item));
    (void)pop(); /* room again */
    TEST_ASSERT_EQUAL_INT(CLI_RX_DROPPED_LATCHED, cli_rx_offer(&latch, 3, push, &item));
    TEST_ASSERT_EQUAL_INT(0, q.n);
    cli_latch_clear(&latch); /* a generation change or the worker clears it */
    TEST_ASSERT_EQUAL_INT(CLI_RX_QUEUED, cli_rx_offer(&latch, 3, push, &item));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_decision_table);
    RUN_TEST(test_sink_admission);
    RUN_TEST(test_only_a_gone_link_is_a_terminal_tx_failure);
    RUN_TEST(test_resync_lf_after_a_mid_line_transient_failure);
    RUN_TEST(test_rx_security_verdicts);
    RUN_TEST(test_overflow_latch_burst_of_six_plus_reboot);
    RUN_TEST(test_latched_items_are_dropped_even_with_room);
    return UNITY_END();
}
