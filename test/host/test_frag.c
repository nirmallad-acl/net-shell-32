/* TC-UT-03: response fragmentation (REQ-FUNC-09) at MTU 23 and 247. */
#include <string.h>

#include "unity.h"
#include "cli_frag.h"

void setUp(void) {}
void tearDown(void) {}

static void check_concat(size_t total, size_t payload)
{
    uint8_t src[128];
    uint8_t out[128];
    for (size_t i = 0; i < total; i++) {
        src[i] = (uint8_t)(i * 7u + 3u);
    }
    cli_frag_plan_t plan;
    TEST_ASSERT_EQUAL_INT(ESP_OK, cli_frag_plan(total, payload, &plan));
    size_t got = 0;
    for (size_t i = 0; i < plan.count; i++) {
        size_t off = 0;
        size_t len = cli_frag_slice(&plan, i, &off);
        TEST_ASSERT_TRUE(len > 0);
        TEST_ASSERT_TRUE(len <= payload);
        TEST_ASSERT_EQUAL_UINT32(got, off);
        memcpy(&out[off], &src[off], len);
        got += len;
    }
    TEST_ASSERT_EQUAL_UINT32(total, got);
    TEST_ASSERT_EQUAL_MEMORY(src, out, total);
    size_t off = 99;
    TEST_ASSERT_EQUAL_UINT32(0, cli_frag_slice(&plan, plan.count, &off));
    TEST_ASSERT_EQUAL_UINT32(0, off);
}

void test_mtu23_every_length(void)
{
    for (size_t t = 1; t <= 128; t++) {
        check_concat(t, 20);
    }
}

void test_mtu247_every_length(void)
{
    for (size_t t = 1; t <= 128; t++) {
        check_concat(t, 244);
    }
}

void test_small_payload_is_raised_and_args_checked(void)
{
    cli_frag_plan_t plan;
    TEST_ASSERT_EQUAL_INT(ESP_OK, cli_frag_plan(40, 5, &plan));
    TEST_ASSERT_EQUAL_UINT32(20, plan.max_payload);
    TEST_ASSERT_EQUAL_UINT32(2, plan.count);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, cli_frag_plan(0, 20, &plan));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, cli_frag_plan(10, 20, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mtu23_every_length);
    RUN_TEST(test_mtu247_every_length);
    RUN_TEST(test_small_payload_is_raised_and_args_checked);
    return UNITY_END();
}
