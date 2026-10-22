/* TC-UT-07: advertising payloads (REQ-FUNC-78). */
#include <string.h>

#include "unity.h"
#include "adv_payload.h"

void setUp(void) {}
void tearDown(void) {}

/* fb1e4001-54ae-4a28-9f74-dfccb248601d, little-endian (the NETSHELL32-2 OTA service) */
static const uint8_t ota_uuid[16] = { 0x1D, 0x60, 0x48, 0xB2, 0xCC, 0xDF, 0x74, 0x9F,
                                      0x28, 0x4A, 0xAE, 0x54, 0x01, 0x40, 0x1E, 0xFB };
/* 8F3B1A20-5C6D-4E7F-9A0B-1C2D3E4F5A60, little-endian (the CLI service) */
static const uint8_t cli_uuid[16] = { 0x60, 0x5A, 0x4F, 0x3E, 0x2D, 0x1C, 0x0B, 0x9A,
                                      0x7F, 0x4E, 0x6D, 0x5C, 0x20, 0x1A, 0x3B, 0x8F };

void test_adv_data_bytes_match_netshell32_2_for_ota_uuid(void)
{
    uint8_t buf[31];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, adv_build_adv(buf, sizeof(buf), &len, ota_uuid));
    TEST_ASSERT_EQUAL_UINT32(21, len);
    static const uint8_t head[5] = { 0x02, 0x01, 0x06, 0x11, 0x07 }; /* flags 0x06, complete 128-bit list */
    TEST_ASSERT_EQUAL_MEMORY(head, buf, 5);
    TEST_ASSERT_EQUAL_MEMORY(ota_uuid, &buf[5], 16);
}

void test_scan_response_disarmed_is_name_only(void)
{
    uint8_t buf[31];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, adv_build_rsp(buf, sizeof(buf), &len, "NETSHELL32", NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(12, len);
    TEST_ASSERT_EQUAL_UINT8(11, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x09, buf[1]);
    TEST_ASSERT_EQUAL_MEMORY("NETSHELL32", &buf[2], 10);
}

void test_scan_response_armed_has_name_and_cli_uuid_within_31_bytes(void)
{
    uint8_t buf[31];
    size_t len = 0;
    const uint8_t *uu[1] = { cli_uuid };
    TEST_ASSERT_EQUAL_INT(ESP_OK, adv_build_rsp(buf, sizeof(buf), &len, "NETSHELL32", uu, 1));
    TEST_ASSERT_EQUAL_UINT32(30, len);
    TEST_ASSERT_EQUAL_UINT8(17, buf[12]);
    TEST_ASSERT_EQUAL_UINT8(0x07, buf[13]);
    TEST_ASSERT_EQUAL_MEMORY(cli_uuid, &buf[14], 16);
}

void test_too_long_or_bad_args_are_rejected(void)
{
    uint8_t buf[31];
    size_t len = 0;
    const uint8_t *uu[2] = { cli_uuid, ota_uuid };
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, adv_build_rsp(buf, sizeof(buf), &len, "NETSHELL32", uu, 2));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, adv_build_rsp(buf, sizeof(buf), &len, "NETSHELL32XY", uu, 1)); /* 12 chars: 2+12+18 > 31 */
    TEST_ASSERT_EQUAL_INT(ESP_OK, adv_build_rsp(buf, sizeof(buf), &len, "NETSHELL32X", uu, 1)); /* 11 chars: exactly 31 */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, adv_build_adv(buf, 20, &len, ota_uuid));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, adv_build_adv(NULL, 31, &len, ota_uuid));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, adv_build_rsp(buf, sizeof(buf), &len, "x", NULL, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_adv_data_bytes_match_netshell32_2_for_ota_uuid);
    RUN_TEST(test_scan_response_disarmed_is_name_only);
    RUN_TEST(test_scan_response_armed_has_name_and_cli_uuid_within_31_bytes);
    RUN_TEST(test_too_long_or_bad_args_are_rejected);
    return UNITY_END();
}
