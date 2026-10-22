/* TC-UT-02: response grammar and formatting (REQ-FUNC-10, 17, 40, 43, 45). */
#include <string.h>

#include "unity.h"
#include "diag_format.h"

void setUp(void) {}
void tearDown(void) {}

static char buf[128];

static void check_grammar(void)
{
    TEST_ASSERT_TRUE(strlen(buf) <= 127);
    TEST_ASSERT_TRUE(strncmp(buf, "OK ", 3) == 0 || strncmp(buf, "DATA ", 5) == 0 || strncmp(buf, "ERR ", 4) == 0);
    for (const char *p = buf; *p != '\0'; p++) {
        TEST_ASSERT_TRUE((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E);
    }
}

void test_ok_lines(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_ok_kv(buf, sizeof(buf), "hw_version", "Alpha_1.0.0_HW"));
    TEST_ASSERT_EQUAL_STRING("OK hw_version=Alpha_1.0.0_HW", buf);
    check_grammar();
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_ok_kv(buf, sizeof(buf), "fw_version", "1.0.0"));
    TEST_ASSERT_EQUAL_STRING("OK fw_version=1.0.0", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_reboot_reason(buf, sizeof(buf), "software", 3));
    TEST_ASSERT_EQUAL_STRING("OK reboot_reason=software code=3", buf);
    check_grammar();
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_ok_text(buf, sizeof(buf), "rebooting"));
    TEST_ASSERT_EQUAL_STRING("OK rebooting", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_start(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("OK accel_xyz started interval_ms=1000 samples=15", buf);
}

void test_completion_lines(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_complete(buf, sizeof(buf), 15, 0));
    TEST_ASSERT_EQUAL_STRING("OK accel_xyz complete samples=15", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_complete(buf, sizeof(buf), 15, 1));
    TEST_ASSERT_EQUAL_STRING("OK accel_xyz complete samples=15 errors=1", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_complete(buf, sizeof(buf), 15, 15));
    TEST_ASSERT_EQUAL_STRING("OK accel_xyz complete samples=15 errors=15", buf);
}

void test_data_line_and_longest_length(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_data(buf, sizeof(buf), 3, 12, -5, 1000, 0, false));
    TEST_ASSERT_EQUAL_STRING("DATA sample=3 x=0.012 y=-0.005 z=1.000 unit=g age_ms=0 source=simulated", buf);
    check_grammar();
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_data(buf, sizeof(buf), 15, -2000, -2000, -2000, 100, false));
    TEST_ASSERT_EQUAL_UINT32(76, strlen(buf)); /* the longest DATA line of the spec */
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_data(buf, sizeof(buf), 1, 1, 2, 3, 4, true));
    TEST_ASSERT_NOT_NULL(strstr(buf, "source=hardware"));
}

void test_milli_g_rounding_and_sign(void)
{
    char s[16];
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_mg(s, sizeof(s), 0));
    TEST_ASSERT_EQUAL_STRING("0.000", s);
    diag_fmt_mg(s, sizeof(s), -1);
    TEST_ASSERT_EQUAL_STRING("-0.001", s);
    diag_fmt_mg(s, sizeof(s), 1993);
    TEST_ASSERT_EQUAL_STRING("1.993", s);
    diag_fmt_mg(s, sizeof(s), -2000);
    TEST_ASSERT_EQUAL_STRING("-2.000", s);
    TEST_ASSERT_EQUAL_INT32(4, diag_g_to_mg(0.0039f));
    TEST_ASSERT_EQUAL_INT32(-4, diag_g_to_mg(-0.0039f));
    TEST_ASSERT_EQUAL_INT32(250, diag_g_to_mg(0.250f));
    TEST_ASSERT_EQUAL_INT32(0, diag_g_to_mg(0.0004f));
    TEST_ASSERT_EQUAL_INT32(0, diag_g_to_mg(-0.0004f)); /* never prints -0.000 */
    diag_fmt_mg(s, sizeof(s), diag_g_to_mg(-0.0004f));
    TEST_ASSERT_EQUAL_STRING("0.000", s);
}

void test_error_grammar_and_closed_code_set(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_err(buf, sizeof(buf), "unavailable", "hw_version", NULL));
    TEST_ASSERT_EQUAL_STRING("ERR unavailable cmd=hw_version", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_err(buf, sizeof(buf), "busy", "reboot", "reason=ota_active"));
    TEST_ASSERT_EQUAL_STRING("ERR busy cmd=reboot reason=ota_active", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_err(buf, sizeof(buf), "unknown_command", NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("ERR unknown_command", buf);
    TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_err(buf, sizeof(buf), "sensor_error", 5));
    TEST_ASSERT_EQUAL_STRING("ERR sensor_error cmd=accel_xyz sample=5", buf);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, diag_fmt_err(buf, sizeof(buf), "made_up_code", NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_truncation_and_non_printable_are_errors(void)
{
    char small[10];
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, diag_fmt_ok_kv(small, sizeof(small), "hw_version", "Alpha_1.0.0_HW"));
    TEST_ASSERT_EQUAL_STRING("", small);
    char longval[200];
    memset(longval, 'a', sizeof(longval) - 1);
    longval[sizeof(longval) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, diag_fmt_ok_kv(buf, sizeof(buf), "k", longval));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, diag_fmt_ok_kv(buf, sizeof(buf), "k", "a\tb"));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, diag_fmt_ok_kv(NULL, 10, "k", "v"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ok_lines);
    RUN_TEST(test_completion_lines);
    RUN_TEST(test_data_line_and_longest_length);
    RUN_TEST(test_milli_g_rounding_and_sign);
    RUN_TEST(test_error_grammar_and_closed_code_set);
    RUN_TEST(test_truncation_and_non_printable_are_errors);
    return UNITY_END();
}
