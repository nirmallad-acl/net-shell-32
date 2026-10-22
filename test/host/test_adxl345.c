/* TC-UT-12b: ADXL345 register-level driver against a mock I2C bus
 * (REQ-FUNC-50, 51, 52, REQ-TEST-01, REQ-HW-06). Mock results are NOT validation of
 * the real part (HW-01, HW-06 stay PENDING). */
#include <string.h>

#include "unity.h"
#include "adxl345.h"
#include "adxl345_regs.h"
#include "mock_bus.h"

static mock_bus_t bus;

void setUp(void)
{
    mock_bus_reset(&bus);
    bus.present_addr = 0x53;
    bus.regs[0x00] = 0xE5;
}

void tearDown(void) {}

static adxl345_t dev(void)
{
    adxl345_t d = { .ops = &mock_bus_ops, .bus = &bus, .addr = 0x53, .timeout_ms = 50 };
    return d;
}

/* identity + the 8 configuration transactions; returns the first error */
static esp_err_t run_all(const adxl345_t *d)
{
    esp_err_t e = adxl345_check_identity(d);
    for (unsigned s = 0; e == ESP_OK && s < ADXL345_CFG_STEPS; s++) {
        e = adxl345_config_step(d, s);
    }
    return e;
}

void test_identity_accepts_only_0xe5_and_never_writes(void)
{
    adxl345_t d = dev();
    TEST_ASSERT_EQUAL_INT(ESP_OK, adxl345_check_identity(&d));
    static const uint8_t bad[] = { 0x00, 0xFF, 0xE6, 0xE4 };
    for (size_t i = 0; i < sizeof(bad); i++) {
        bus.regs[0x00] = bad[i];
        TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_IDENTITY, adxl345_check_identity(&d));
    }
    TEST_ASSERT_EQUAL_UINT32(0, bus.n_writes); /* no configuration write before identity passes */
}

void test_configuration_sequence_and_values(void)
{
    adxl345_t d = dev();
    TEST_ASSERT_EQUAL_INT(ESP_OK, run_all(&d));
    /* log[0] is the DEVID read; then 6 writes, then 2 read-backs */
    static const struct { uint8_t reg, val; } want[6] = {
        { 0x2D, 0x00 }, { 0x2C, 0x0A }, { 0x31, 0x08 }, { 0x38, 0x00 }, { 0x2E, 0x00 }, { 0x2D, 0x08 },
    };
    TEST_ASSERT_EQUAL_UINT32(1 + 6 + 2, bus.n_ops);
    TEST_ASSERT_EQUAL_CHAR('R', bus.log[0].kind);
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_CHAR('W', bus.log[1 + i].kind);
        TEST_ASSERT_EQUAL_HEX8(want[i].reg, bus.log[1 + i].reg);
        TEST_ASSERT_EQUAL_HEX8(want[i].val, bus.log[1 + i].val);
    }
    TEST_ASSERT_EQUAL_CHAR('R', bus.log[7].kind);
    TEST_ASSERT_EQUAL_HEX8(0x2C, bus.log[7].reg);
    TEST_ASSERT_EQUAL_CHAR('R', bus.log[8].kind);
    TEST_ASSERT_EQUAL_HEX8(0x31, bus.log[8].reg);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, adxl345_config_step(&d, ADXL345_CFG_STEPS));
}

void test_readback_mismatch_is_config_error(void)
{
    adxl345_t d = dev();
    for (unsigned s = 0; s < 6; s++) {
        TEST_ASSERT_EQUAL_INT(ESP_OK, adxl345_config_step(&d, s));
    }
    bus.regs[0x2C] = 0x0B; /* the part did not take BW_RATE */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_config_step(&d, 6));
    bus.regs[0x2C] = 0x0A;
    bus.regs[0x31] = 0x0B;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_config_step(&d, 7));
}

void test_decode_vectors_and_sign_extension(void)
{
    int16_t v[3];
    const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    adxl345_decode(zero, v);
    TEST_ASSERT_EQUAL_INT16(0, v[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.00196f, 0.0f, adxl345_raw_to_g(0));

    const uint8_t vec[6] = { 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0x01 }; /* +1, -1, +511 */
    adxl345_decode(vec, v);
    TEST_ASSERT_EQUAL_INT16(1, v[0]);
    TEST_ASSERT_EQUAL_INT16(-1, v[1]);
    TEST_ASSERT_EQUAL_INT16(511, v[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.00196f, 0.004f, adxl345_raw_to_g(v[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00196f, -0.004f, adxl345_raw_to_g(v[1]));
    TEST_ASSERT_FLOAT_WITHIN(0.00196f, 1.993f, adxl345_raw_to_g(v[2]));

    const uint8_t neg[6] = { 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00 }; /* -512 */
    adxl345_decode(neg, v);
    TEST_ASSERT_EQUAL_INT16(-512, v[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.00196f, -1.997f, adxl345_raw_to_g(v[0]));
}

static void configure_regs(void)
{
    bus.regs[0x2C] = 0x0A; /* BW_RATE, as left by the configuration sequence */
    bus.regs[0x2D] = 0x08; /* POWER_CTL measure */
    bus.regs[0x31] = 0x08; /* DATA_FORMAT FULL_RES +/-2 g */
    bus.regs[0x38] = 0x00; /* FIFO_CTL bypass */
}

void test_burst_read_is_one_twelve_byte_transaction_from_0x2c(void)
{
    adxl345_t d = dev();
    configure_regs();
    const uint8_t data[6] = { 0x10, 0x00, 0xF0, 0xFF, 0x00, 0x01 };
    memcpy(&bus.regs[0x32], data, 6);
    int16_t v[3];
    TEST_ASSERT_EQUAL_INT(ESP_OK, adxl345_read_raw(&d, v));
    TEST_ASSERT_EQUAL_INT16(16, v[0]);
    TEST_ASSERT_EQUAL_INT16(-16, v[1]);
    TEST_ASSERT_EQUAL_INT16(256, v[2]);
    TEST_ASSERT_EQUAL_UINT32(1, bus.n_ops); /* coherent X/Y/Z AND configuration from one transaction */
    TEST_ASSERT_EQUAL_UINT32(13, bus.log[0].n); /* 0x2C..0x38, includes FIFO_CTL */
    TEST_ASSERT_EQUAL_HEX8(0x2C, bus.log[0].reg);
}

/* ADV-IMPL-02: a part that reset (POWER_CTL = 0, data 0) or lost any configuration must
 * never yield a sample: ESP_ERR_ACCEL_CONFIG and the output is not written. */
void test_reset_standby_or_wrong_config_is_never_a_sample(void)
{
    adxl345_t d = dev();
    int16_t v[3] = { 111, 222, 333 };

    /* power-on-reset values: BW_RATE 0x0A is the datasheet reset value, POWER_CTL 0, DATA_FORMAT 0 */
    memset(&bus.regs[0x2C], 0, 13);
    bus.regs[0x2C] = 0x0A;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));
    TEST_ASSERT_EQUAL_INT16(111, v[0]); /* untouched */

    /* ADV-R2-03: sleep bit set (even with measure set) is not a running part */
    configure_regs();
    bus.regs[0x2D] = 0x08 | 0x04;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));

    /* ADV-R2-03: FIFO mode other than bypass (FIFO 0x40, stream 0x80, trigger 0xC0) fails;
     * the low samples-bits of FIFO_CTL do not matter */
    static const uint8_t bad_fifo[] = { 0x40, 0x80, 0xC0, 0x9F };
    for (size_t i = 0; i < sizeof(bad_fifo); i++) {
        configure_regs();
        bus.regs[0x38] = bad_fifo[i];
        TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));
        TEST_ASSERT_EQUAL_INT16(111, v[0]);
    }
    configure_regs();
    bus.regs[0x38] = 0x1F; /* bypass with a samples field: still bypass */
    TEST_ASSERT_EQUAL_INT(ESP_OK, adxl345_read_raw(&d, v));

    configure_regs();
    bus.regs[0x2D] = 0x00; /* standby */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));
    configure_regs();
    bus.regs[0x2D] = 0x00 | 0x04; /* sleep/other bit set but measure bit clear */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));
    configure_regs();
    bus.regs[0x2C] = 0x0B; /* different data rate */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));
    configure_regs();
    bus.regs[0x31] = 0x00; /* 10-bit mode */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, adxl345_read_raw(&d, v));

    configure_regs();
    bus.regs[0x2D] = 0x08 | 0x20; /* extra non-measure bits are fine as long as measure is set */
    TEST_ASSERT_EQUAL_INT(ESP_OK, adxl345_read_raw(&d, v));
}

/* NACK / timeout / bus error at every transaction: a distinct status, and the
 * sequence stops at the failing step. */
void test_errors_at_every_step_are_reported_distinctly(void)
{
    static const esp_err_t codes[3] = { ESP_ERR_ACCEL_NOT_PRESENT, ESP_ERR_ACCEL_TIMEOUT, ESP_ERR_ACCEL_BUS };
    for (size_t k = 0; k < 1 + ADXL345_CFG_STEPS; k++) {
        for (int c = 0; c < 3; c++) {
            mock_bus_reset(&bus);
            bus.present_addr = 0x53;
            bus.regs[0x00] = 0xE5;
            bus.script[k] = codes[c];
            bus.script_len = k + 1;
            adxl345_t d = dev();
            TEST_ASSERT_EQUAL_INT(codes[c], run_all(&d));
            TEST_ASSERT_EQUAL_UINT32(k + 1, bus.n_ops);
        }
    }
}

void test_foreign_bus_codes_are_folded(void)
{
    adxl345_t d = dev();
    bus.fail_all = ESP_FAIL;
    int16_t v[3];
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_READ, adxl345_read_raw(&d, v));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_BUS, adxl345_check_identity(&d));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, adxl345_read_raw(&d, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_accepts_only_0xe5_and_never_writes);
    RUN_TEST(test_configuration_sequence_and_values);
    RUN_TEST(test_readback_mismatch_is_config_error);
    RUN_TEST(test_decode_vectors_and_sign_extension);
    RUN_TEST(test_burst_read_is_one_twelve_byte_transaction_from_0x2c);
    RUN_TEST(test_reset_standby_or_wrong_config_is_never_a_sample);
    RUN_TEST(test_errors_at_every_step_are_reported_distinctly);
    RUN_TEST(test_foreign_bus_codes_are_folded);
    return UNITY_END();
}
