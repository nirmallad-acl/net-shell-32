/* TC-UT-11 (schedule part): deadlines, slot classification, error counting
 * (REQ-FUNC-40, 41, 44, 45, 56). */
#include "unity.h"
#include "accel_sched.h"
#include "diag_format.h"

void setUp(void) {}
void tearDown(void) {}

void test_deadlines_are_absolute_not_cumulative(void)
{
    const int64_t t0 = 5000000;
    TEST_ASSERT_EQUAL_INT64(t0, accel_sched_deadline_us(t0, 1));
    TEST_ASSERT_EQUAL_INT64(t0 + 1000000, accel_sched_deadline_us(t0, 2));
    TEST_ASSERT_EQUAL_INT64(t0 + 14000000, accel_sched_deadline_us(t0, 15));
    TEST_ASSERT_EQUAL_INT64(t0, accel_sched_deadline_us(t0, 0));
    TEST_ASSERT_EQUAL_INT64(t0, accel_sched_deadline_us(t0, 16));
}

void test_age_floor_and_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, accel_sched_age_ms(1000, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, accel_sched_age_ms(2000, 1000)); /* clock skew never goes negative */
    TEST_ASSERT_EQUAL_UINT32(100, accel_sched_age_ms(0, 100999));
    TEST_ASSERT_EQUAL_UINT32(101, accel_sched_age_ms(0, 101000));
}

void test_simulated_is_never_an_error(void)
{
    uint32_t age = 99;
    TEST_ASSERT_EQUAL_INT(ACCEL_SLOT_SIM_DATA, accel_sched_classify(false, ESP_OK, 0, 5, 100, &age));
    TEST_ASSERT_EQUAL_UINT32(0, age);
    TEST_ASSERT_EQUAL_INT(ACCEL_SLOT_SIM_DATA, accel_sched_classify(false, ESP_OK, 0, 900000, 100, NULL));
}

void test_hardware_age_limit_inclusive(void)
{
    const int64_t lim = 100;
    TEST_ASSERT_EQUAL_INT(ACCEL_SLOT_HW_DATA, accel_sched_classify(true, ESP_OK, 0, lim * 1000, 100, NULL));
    TEST_ASSERT_EQUAL_INT(ACCEL_SLOT_ERR_STALE, accel_sched_classify(true, ESP_OK, 0, (lim + 1) * 1000, 100, NULL));
}

void test_hardware_error_is_sensor_error(void)
{
    TEST_ASSERT_EQUAL_INT(ACCEL_SLOT_ERR_SENSOR, accel_sched_classify(true, ESP_FAIL, 0, 0, 100, NULL));
    TEST_ASSERT_TRUE(accel_slot_is_error(ACCEL_SLOT_ERR_SENSOR));
    TEST_ASSERT_TRUE(accel_slot_is_error(ACCEL_SLOT_ERR_STALE));
    TEST_ASSERT_FALSE(accel_slot_is_error(ACCEL_SLOT_HW_DATA));
    TEST_ASSERT_FALSE(accel_slot_is_error(ACCEL_SLOT_SIM_DATA));
}

/* k = 0, 1, 15 errors give the exact completion line. */
void test_completion_line_for_k_errors(void)
{
    char buf[128];
    static const uint32_t ks[] = { 0, 1, 15 };
    static const char *const want[] = { "OK accel_xyz complete samples=15", "OK accel_xyz complete samples=15 errors=1",
                                        "OK accel_xyz complete samples=15 errors=15" };
    for (int i = 0; i < 3; i++) {
        uint32_t errors = 0;
        for (uint32_t n = 1; n <= ACCEL_SESSION_SAMPLES; n++) {
            accel_slot_t s = accel_sched_classify(true, (n <= ks[i]) ? ESP_FAIL : ESP_OK, 0, 0, 100, NULL);
            if (accel_slot_is_error(s)) {
                errors++;
            }
        }
        TEST_ASSERT_EQUAL_UINT32(ks[i], errors);
        TEST_ASSERT_EQUAL_INT(ESP_OK, diag_fmt_accel_complete(buf, sizeof(buf), ACCEL_SESSION_SAMPLES, errors));
        TEST_ASSERT_EQUAL_STRING(want[i], buf);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_deadlines_are_absolute_not_cumulative);
    RUN_TEST(test_age_floor_and_clamp);
    RUN_TEST(test_simulated_is_never_an_error);
    RUN_TEST(test_hardware_age_limit_inclusive);
    RUN_TEST(test_hardware_error_is_sensor_error);
    RUN_TEST(test_completion_line_for_k_errors);
    return UNITY_END();
}
