/* TC-UT-10 / TC-UT-04 (generation part): the generation filter on both sides of the
 * BLE CLI (REQ-FUNC-62, 63, 64). */
#include <string.h>

#include "unity.h"
#include "cli_line_asm.h"
#include "cli_tx_gate.h"
#include "host_registry.h"

void setUp(void) {}
void tearDown(void) {}

#define MAXR 8
static cli_result_t g_res[MAXR];
static size_t g_n;

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

static void collect(const cli_result_t *r, void *u)
{
    (void)u;
    if (g_n < MAXR) {
        g_res[g_n] = *r;
    }
    g_n++;
}

/* REQ-FUNC-62: `reb` + reconnect (new generation) + `oot\n` must NOT run reboot. */
void test_partial_line_never_survives_a_generation_change(void)
{
    cli_line_asm_t a;
    g_n = 0;
    cli_line_asm_reset(&a, 1);
    cli_line_asm_feed(&a, (const uint8_t *)"reb", 3, lookup, collect, NULL);
    TEST_ASSERT_TRUE(cli_line_asm_sync(&a, 2)); /* the RX callback syncs before feeding */
    cli_line_asm_feed(&a, (const uint8_t *)"oot\n", 4, lookup, collect, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_UNKNOWN, g_res[0].kind);
    TEST_ASSERT_FALSE(cli_line_asm_sync(&a, 2)); /* same generation: nothing is discarded */
}

/* A command queued under generation g is never run after the generation moved on. */
void test_command_after_a_bump_is_not_run(void)
{
    const uint32_t queued_gen = 5;
    TEST_ASSERT_TRUE(cli_cmd_should_run(queued_gen, 5));
    TEST_ASSERT_FALSE(cli_cmd_should_run(queued_gen, 6)); /* reconnected */
    TEST_ASSERT_FALSE(cli_cmd_should_run(queued_gen, 0)); /* link gone or not secure */
    TEST_ASSERT_FALSE(cli_cmd_should_run(0, 0));
}

/* Stale response lines are dropped, never delivered to the next connection. */
void test_stale_lines_are_dropped_after_a_bump(void)
{
    TEST_ASSERT_EQUAL_INT(CLI_TX_SEND, cli_tx_decide(5, 5, 0, true));
    TEST_ASSERT_EQUAL_INT(CLI_TX_DROP_STALE, cli_tx_decide(5, 6, 0, true));
    TEST_ASSERT_FALSE(cli_sink_admit(5, 6, 0));
}

/* REQ-FUNC-64: increments on connect/disconnect/security loss, skips 0 on wrap, so
 * (generation 0 is the "no link" sentinel) a stale value can never equal a live one. */
void test_generation_sequence_and_wrap(void)
{
    host_reg_t r;
    host_reg_init(&r, 2);
    uint32_t g1 = host_reg_gen(&r);
    uint32_t g2 = host_reg_next_gen(&r); /* connect */
    uint32_t g3 = host_reg_next_gen(&r); /* secure -> insecure */
    uint32_t g4 = host_reg_next_gen(&r); /* disconnect */
    TEST_ASSERT_TRUE(g1 != g2 && g2 != g3 && g3 != g4);
    r.gen = UINT32_MAX - 1u;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_reg_next_gen(&r));
    TEST_ASSERT_EQUAL_UINT32(1, host_reg_next_gen(&r)); /* 0 skipped */
    TEST_ASSERT_TRUE(host_reg_gen(&r) != 0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_partial_line_never_survives_a_generation_change);
    RUN_TEST(test_command_after_a_bump_is_not_run);
    RUN_TEST(test_stale_lines_are_dropped_after_a_bump);
    RUN_TEST(test_generation_sequence_and_wrap);
    return UNITY_END();
}
