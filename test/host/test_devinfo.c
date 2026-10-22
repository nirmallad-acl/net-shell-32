/* TC-UT-14: hardware revision / firmware version rules (REQ-FUNC-20, 22, 25, 26, 33, 34).
 * Covers the PURE validation module. The NVS read/write glue and the app descriptor
 * access in device_info.c are covered by E2E-SER (TC-SER-02/03/04/08). */
#include <string.h>

#include "unity.h"
#include "devinfo_validate.h"

void setUp(void) {}
void tearDown(void) {}

void test_hw_rev_accepts_the_board_value_and_the_charset(void)
{
    TEST_ASSERT_TRUE(devinfo_validate_hw_rev("Alpha_1.0.0_HW"));
    TEST_ASSERT_TRUE(devinfo_validate_hw_rev("A"));
    TEST_ASSERT_TRUE(devinfo_validate_hw_rev("a-b_c.D9"));
}

void test_hw_rev_length_boundaries(void)
{
    char s[40];
    memset(s, 'x', 32);
    s[32] = '\0';
    TEST_ASSERT_TRUE(devinfo_validate_hw_rev(s));  /* 32 accepted */
    s[32] = 'x';
    s[33] = '\0';
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev(s)); /* 33 rejected */
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev(""));
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev(NULL));
}

void test_hw_rev_rejects_bad_characters(void)
{
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev("bad/rev"));
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev("has space"));
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev("tab\t"));
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev("plus+sign")); /* '+' is allowed for fw versions only */
    TEST_ASSERT_FALSE(devinfo_validate_hw_rev("caf\xC3\xA9"));
}

void test_fw_version_valid_and_placeholders(void)
{
    TEST_ASSERT_TRUE(devinfo_validate_fw_version("1.0.0", 32, 31));
    TEST_ASSERT_TRUE(devinfo_validate_fw_version("1.0.1-test", 32, 31));
    TEST_ASSERT_TRUE(devinfo_validate_fw_version("2.3.4+build5", 32, 31));
    TEST_ASSERT_FALSE(devinfo_validate_fw_version("1", 32, 31));  /* IDF placeholder */
    TEST_ASSERT_FALSE(devinfo_validate_fw_version("", 32, 31));
    TEST_ASSERT_FALSE(devinfo_validate_fw_version(NULL, 32, 31));
    TEST_ASSERT_TRUE(devinfo_validate_fw_version("10", 32, 31));  /* only the exact "1" is a placeholder */
}

void test_fw_version_length_and_charset_and_termination(void)
{
    char s[40];
    memset(s, '1', 31);
    s[31] = '\0';
    TEST_ASSERT_TRUE(devinfo_validate_fw_version(s, 32, 31));
    memset(s, '1', 32);
    s[32] = '\0';
    TEST_ASSERT_FALSE(devinfo_validate_fw_version(s, 40, 31)); /* 32 characters */
    TEST_ASSERT_FALSE(devinfo_validate_fw_version("1.0 0", 32, 31));
    TEST_ASSERT_FALSE(devinfo_validate_fw_version("1.0/0", 32, 31));
    const char unterminated[4] = { '1', '.', '0', '1' };
    TEST_ASSERT_FALSE(devinfo_validate_fw_version(unterminated, sizeof(unterminated), 31)); /* never over-reads */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hw_rev_accepts_the_board_value_and_the_charset);
    RUN_TEST(test_hw_rev_length_boundaries);
    RUN_TEST(test_hw_rev_rejects_bad_characters);
    RUN_TEST(test_fw_version_valid_and_placeholders);
    RUN_TEST(test_fw_version_length_and_charset_and_termination);
    return UNITY_END();
}
