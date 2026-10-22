/* TC-UT-15: reset reason token table (REQ-FUNC-27, 28). The enum values are the
 * ESP-IDF v6.0.2 esp_reset_reason_t numbers; the target build ties the table length
 * to ESP_RST_CPU_LOCKUP with a _Static_assert in device_info.c. */
#include <string.h>

#include "unity.h"
#include "reset_reason_map.h"

void setUp(void) {}
void tearDown(void) {}

void test_every_code_maps_to_the_documented_token(void)
{
    static const char *const want[16] = {
        "unknown", "power_on", "external", "software", "panic", "int_wdt", "task_wdt", "wdt",
        "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "power_glitch", "cpu_lockup",
    };
    TEST_ASSERT_EQUAL_INT(16, RESET_REASON_MAP_COUNT);
    for (int c = 0; c < 16; c++) {
        const char *t = reset_reason_token(c);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 0);
        TEST_ASSERT_EQUAL_STRING(want[c], t);
    }
}

void test_out_of_range_is_unknown_and_never_out_of_bounds(void)
{
    TEST_ASSERT_EQUAL_STRING("unknown", reset_reason_token(16));
    TEST_ASSERT_EQUAL_STRING("unknown", reset_reason_token(0xFFFF));
    TEST_ASSERT_EQUAL_STRING("unknown", reset_reason_token(-1));
    TEST_ASSERT_EQUAL_STRING("unknown", reset_reason_token(2147483647));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_code_maps_to_the_documented_token);
    RUN_TEST(test_out_of_range_is_unknown_and_never_out_of_bounds);
    return UNITY_END();
}
