/* TC-UT-01: line assembler (REQ-FUNC-05..08, 13, 16, REQ-SEC-02, REQ-SEC-03). */
#include <string.h>

#include "unity.h"
#include "cli_line_asm.h"

void setUp(void) {}
void tearDown(void) {}

#define MAXR 512
static cli_result_t g_res[MAXR];
static size_t       g_n;

static const char *const s_names[5] = { "hw_version", "fw_version", "accel_xyz", "reboot_reason", "reboot" };

static int lookup(const char *name, size_t len)
{
    for (int i = 0; i < 5; i++) {
        if (strlen(s_names[i]) == len && memcmp(s_names[i], name, len) == 0) {
            return i;
        }
    }
    return -1;
}

static void collect(const cli_result_t *r, void *user)
{
    (void)user;
    if (g_n < MAXR) {
        g_res[g_n] = *r;
    }
    g_n++;
}

static cli_line_asm_t a;

static void begin(void)
{
    g_n = 0;
    cli_line_asm_reset(&a, 1);
}

static size_t feed(const void *p, size_t n)
{
    return cli_line_asm_feed(&a, (const uint8_t *)p, n, lookup, collect, NULL);
}

static size_t feed_s(const char *s)
{
    return feed(s, strlen(s));
}

void test_single_command(void)
{
    begin();
    TEST_ASSERT_EQUAL_UINT32(1, feed_s("hw_version\n"));
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[0].kind);
    TEST_ASSERT_EQUAL_INT(0, g_res[0].cmd_id);
}

void test_split_across_writes_and_crlf(void)
{
    begin();
    feed_s("hw_");
    feed_s("ver");
    TEST_ASSERT_EQUAL_UINT32(0, g_n);
    feed_s("sion\r\n");
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[0].kind);
}

void test_two_commands_in_one_write_in_order(void)
{
    begin();
    feed_s("hw_version\nfw_version\n");
    TEST_ASSERT_EQUAL_UINT32(2, g_n);
    TEST_ASSERT_EQUAL_INT(0, g_res[0].cmd_id);
    TEST_ASSERT_EQUAL_INT(1, g_res[1].cmd_id);
}

void test_partial_line_no_output(void)
{
    begin();
    feed_s("hw_version");
    TEST_ASSERT_EQUAL_UINT32(0, g_n);
}

void test_empty_and_whitespace_lines(void)
{
    begin();
    feed_s("\n");
    feed_s("\r\n");
    feed_s("   \n");
    feed_s("\t \r\n");
    TEST_ASSERT_EQUAL_UINT32(0, g_n);
}

void test_trim_and_case(void)
{
    begin();
    feed_s("  hw_version \n");
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[0].kind);
    begin();
    feed_s("HW_VERSION\n");
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_UNKNOWN, g_res[0].kind);
}

void test_oversized_65_byte_line_ending_reboot(void)
{
    begin();
    char line[80];
    memset(line, 'a', 59);
    memcpy(&line[59], "reboot", 6); /* 65 bytes, ends with "reboot" */
    line[65] = '\n';
    feed(line, 66);
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_OVERSIZED, g_res[0].kind);
    feed_s("hw_version\n"); /* the next line is fine */
    TEST_ASSERT_EQUAL_UINT32(2, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[1].kind);
}

void test_exactly_64_bytes_is_not_oversized(void)
{
    begin();
    char line[80];
    memset(line, ' ', 64);
    memcpy(&line[54], "hw_version", 10);
    line[64] = '\r';
    line[65] = '\n';
    feed(line, 66);
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[0].kind);
}

void test_oversized_200_bytes_starting_reboot(void)
{
    begin();
    char line[210];
    memset(line, 'a', sizeof(line));
    memcpy(line, "reboot", 6);
    line[200] = '\n';
    feed(line, 201);
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_OVERSIZED, g_res[0].kind);
    feed_s("hw_version\n");
    TEST_ASSERT_EQUAL_UINT32(2, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_CMD, g_res[1].kind);
}

void test_invalid_characters(void)
{
    begin();
    static const uint8_t nul_line[] = { 'h', 'w', '_', 0x00, 'v', 'e', 'r', 's', 'i', 'o', 'n', '\n' };
    feed(nul_line, sizeof(nul_line));
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_INVALID_CHAR, g_res[0].kind);
    begin();
    static const uint8_t ff_line[] = { 'h', 'w', '_', 'v', 'e', 'r', 's', 'i', 'o', 'n', 0xFF, '\n' };
    feed(ff_line, sizeof(ff_line));
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_INVALID_CHAR, g_res[0].kind);
}

void test_oversized_beats_invalid_char(void)
{
    begin();
    uint8_t line[101];
    memset(line, 'a', 100);
    line[50] = 0xFF;
    line[100] = '\n';
    feed(line, 101);
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_OVERSIZED, g_res[0].kind);
}

void test_arguments_rejected_with_command_name(void)
{
    begin();
    feed_s("hw_version x\n");
    feed_s("reboot now\n");
    feed_s("accel_xyz 5\n");
    TEST_ASSERT_EQUAL_UINT32(3, g_n);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_ARGS, g_res[i].kind);
    }
    TEST_ASSERT_EQUAL_INT(0, g_res[0].cmd_id);
    TEST_ASSERT_EQUAL_INT(4, g_res[1].cmd_id);
    TEST_ASSERT_EQUAL_INT(2, g_res[2].cmd_id);
}

void test_serial_only_names_are_unknown(void)
{
    static const char *const names[] = { "help", "ifconfig", "wifiscan", "wifi_connect", "ping", "fwupgrade",
                                         "hw_rev_set X" };
    begin();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        feed_s(names[i]);
        feed_s("\n");
    }
    TEST_ASSERT_EQUAL_UINT32(7, g_n);
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_UNKNOWN, g_res[i].kind);
    }
}

void test_generation_reset_discards_partial_line(void)
{
    begin();
    feed_s("reb");
    cli_line_asm_reset(&a, 2); /* a generation change (REQ-FUNC-62) */
    feed_s("oot\n");
    TEST_ASSERT_EQUAL_UINT32(1, g_n);
    TEST_ASSERT_EQUAL_INT(CLI_KIND_ERR_UNKNOWN, g_res[0].kind);
}

/* 10,000 random writes of 0..512 bytes, fixed seed: no crash, and a command can
 * only come out of a complete valid line. */
void test_fuzz_no_crash_no_phantom_command(void)
{
    uint32_t s = 0x1234567u;
    static uint8_t buf[512];
    size_t newlines = 0;
    begin();
    for (int w = 0; w < 10000; w++) {
        s = s * 1664525u + 1013904223u;
        size_t n = (s >> 8) % 513u;
        for (size_t i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            uint8_t b = (uint8_t)(s >> 16);
            if ((s >> 29) == 0u) { /* bias towards terminators and printable text */
                b = '\n';
            } else if ((s >> 29) < 4u) {
                b = (uint8_t)(0x20u + ((s >> 8) % 0x5Fu));
            }
            if (b == '\n') {
                newlines++;
            }
            buf[i] = b;
        }
        feed(buf, n);
    }
    size_t cmds = 0;
    for (size_t i = 0; i < g_n && i < MAXR; i++) {
        if (g_res[i].kind == CLI_KIND_CMD) {
            cmds++;
        }
    }
    TEST_ASSERT_TRUE(cmds <= newlines);
    TEST_ASSERT_TRUE(g_n <= newlines);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_command);
    RUN_TEST(test_split_across_writes_and_crlf);
    RUN_TEST(test_two_commands_in_one_write_in_order);
    RUN_TEST(test_partial_line_no_output);
    RUN_TEST(test_empty_and_whitespace_lines);
    RUN_TEST(test_trim_and_case);
    RUN_TEST(test_oversized_65_byte_line_ending_reboot);
    RUN_TEST(test_exactly_64_bytes_is_not_oversized);
    RUN_TEST(test_oversized_200_bytes_starting_reboot);
    RUN_TEST(test_invalid_characters);
    RUN_TEST(test_oversized_beats_invalid_char);
    RUN_TEST(test_arguments_rejected_with_command_name);
    RUN_TEST(test_serial_only_names_are_unknown);
    RUN_TEST(test_generation_reset_discards_partial_line);
    RUN_TEST(test_fuzz_no_crash_no_phantom_command);
    return UNITY_END();
}
