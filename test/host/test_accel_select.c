/* TC-UT-13: sensor state machine over a mock bus, mock lock and mock clock
 * (REQ-FUNC-44, 57, 58, REQ-PERF-05, REQ-HW-04, REQ-HW-08, REQ-TEST-02).
 * Mock results are NOT validation of the real part (HW-03, HW-07, HW-08 stay PENDING). */
#include <string.h>

#include "unity.h"
#include "accel_select.h"
#include "adxl345_regs.h"
#include "mock_bus.h"

static mock_bus_t bus;
static accel_sel_t sel;

/* mock lock: records the hold time in mock (bus) time and the acquisition count */
static int64_t g_lock_t0;
static int64_t g_max_hold_us;
static unsigned g_lock_count;
static bool g_lock_fail;

#define MAX_EVT 32
static struct { accel_sel_evt_t evt; uint8_t addr; esp_err_t err; } g_evts[MAX_EVT];
static size_t g_n_evts;

static bool env_lock(void *c, uint32_t ms)
{
    (void)c;
    (void)ms;
    if (g_lock_fail) {
        return false;
    }
    g_lock_count++;
    g_lock_t0 = bus.now_us;
    return true;
}

static void env_unlock(void *c)
{
    (void)c;
    int64_t hold = bus.now_us - g_lock_t0;
    if (hold > g_max_hold_us) {
        g_max_hold_us = hold;
    }
}

static int64_t env_now(void *c)
{
    (void)c;
    return bus.now_us;
}

static bool g_wake; /* what the production notify callback must NOT do for the probe task itself */

static void env_notify(void *c, accel_sel_evt_t evt, uint8_t addr, esp_err_t err)
{
    (void)c;
    g_wake = true; /* worst case: every event "wakes" the probe task */
    if (g_n_evts < MAX_EVT) {
        g_evts[g_n_evts].evt = evt;
        g_evts[g_n_evts].addr = addr;
        g_evts[g_n_evts].err = err;
    }
    g_n_evts++;
}

static uint32_t g_delay_total_ms;
static size_t   g_ops_at_delay;   /* bus ops issued when the settle delay started */

static void env_delay(void *c, uint32_t ms)
{
    (void)c;
    g_delay_total_ms += ms;
    g_ops_at_delay = bus.n_ops;
    bus.now_us += (int64_t)ms * 1000; /* mock time passes; the lock is NOT held here */
}

static const accel_sel_env_t env = { .ctx = NULL, .lock = env_lock, .unlock = env_unlock, .now_us = env_now,
                                     .notify = env_notify, .delay_ms = env_delay };

static size_t count_evt(accel_sel_evt_t e)
{
    size_t n = 0;
    for (size_t i = 0; i < g_n_evts && i < MAX_EVT; i++) {
        if (g_evts[i].evt == e) {
            n++;
        }
    }
    return n;
}

void setUp(void)
{
    mock_bus_reset(&bus);
    bus.regs[0x00] = 0xE5;
    g_max_hold_us = 0;
    g_lock_count = 0;
    g_lock_fail = false;
    g_n_evts = 0;
    g_wake = false;
    g_delay_total_ms = 0;
    g_ops_at_delay = 0;
    accel_select_init(&sel, &mock_bus_ops, &bus, &env, 50, 3);
}

void tearDown(void) {}

void test_absent_then_present_switches_within_one_cycle(void)
{
    bus.present_addr = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_NOT_PRESENT, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_NO_SENSOR, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_UINT32(0, bus.n_writes); /* nothing is ever written to an absent part */
    bus.present_addr = 0x53;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_UINT8(0x53, accel_select_addr(&sel));
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_UP));
}

void test_alternate_address_is_found(void)
{
    bus.present_addr = 0x1D;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_UINT8(0x1D, accel_select_addr(&sel));
}

void test_probe_results_are_logged_on_change_only(void)
{
    bus.present_addr = 0;
    (void)accel_select_probe_cycle(&sel);
    size_t first = count_evt(ACCEL_SEL_EVT_PROBE);
    TEST_ASSERT_EQUAL_UINT32(2, first); /* one per address */
    (void)accel_select_probe_cycle(&sel);
    (void)accel_select_probe_cycle(&sel);
    TEST_ASSERT_EQUAL_UINT32(first, count_evt(ACCEL_SEL_EVT_PROBE)); /* identical results: silent */
}

void test_wrong_identity_means_no_config_writes_and_no_sensor(void)
{
    bus.present_addr = 0x53;
    bus.regs[0x00] = 0x00;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_IDENTITY, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_NO_SENSOR, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_UINT32(0, bus.n_writes);
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL));
    TEST_ASSERT_EQUAL_UINT32(0, count_evt(ACCEL_SEL_EVT_UP));
}

static void make_active(void)
{
    bus.present_addr = 0x53;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
}

void test_read_when_not_active_never_touches_the_bus(void)
{
    int16_t raw[3];
    int64_t t = 0;
    size_t ops_before = bus.n_ops;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_UINT32(ops_before, bus.n_ops); /* a slow or absent bus never delays a simulated slot */
}

void test_successful_read_returns_data_and_acquisition_time(void)
{
    make_active();
    const uint8_t data[6] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(&bus.regs[0x32], data, 6);
    bus.now_us = 123456;
    int16_t raw[3];
    int64_t t = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT16(16, raw[0]);
    TEST_ASSERT_EQUAL_INT16(256, raw[2]);
    TEST_ASSERT_EQUAL_INT64(123456, t);
}

void test_three_failures_drop_to_no_sensor_then_reprobe_recovers(void)
{
    make_active();
    int16_t raw[3];
    int64_t t = 0;
    bus.fail_all = ESP_ERR_ACCEL_BUS;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_BUS, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_BUS, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel)); /* two strikes: still active */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_BUS, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_NO_SENSOR, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL));
    size_t ops_before = bus.n_ops;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, accel_select_read(&sel, raw, &t)); /* later slots: simulated */
    TEST_ASSERT_EQUAL_UINT32(ops_before, bus.n_ops);

    bus.fail_all = 0; /* the part comes back */
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
}

void test_a_success_resets_the_failure_count(void)
{
    make_active();
    int16_t raw[3];
    int64_t t = 0;
    bus.fail_all = ESP_ERR_ACCEL_TIMEOUT;
    (void)accel_select_read(&sel, raw, &t);
    (void)accel_select_read(&sel, raw, &t);
    bus.fail_all = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_read(&sel, raw, &t));
    bus.fail_all = ESP_ERR_ACCEL_TIMEOUT;
    (void)accel_select_read(&sel, raw, &t);
    (void)accel_select_read(&sel, raw, &t);
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel)); /* never 3 in a row */
}

void test_lock_timeout_is_a_busy_failure(void)
{
    make_active();
    g_lock_fail = true;
    int16_t raw[3];
    int64_t t = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_BUSY, accel_select_read(&sel, raw, &t));
}

/* REQ-PERF-05 / REQ-HW-08: always-timeout bus: every lock hold is one transaction
 * (<= 55 ms), each bus operation has its own acquisition, and at most one bus
 * recover() runs per probe cycle. */
void test_lock_hold_bound_one_op_per_lock_and_one_recover_per_cycle(void)
{
    bus.present_addr = 0x53;
    bus.fail_all = ESP_ERR_ACCEL_TIMEOUT;
    bus.latency_ms_on_timeout = 50;
    for (int cycle = 1; cycle <= 3; cycle++) {
        (void)accel_select_probe_cycle(&sel);
        TEST_ASSERT_EQUAL_UINT32((size_t)cycle, bus.n_recovers);
    }
    TEST_ASSERT_TRUE(g_max_hold_us <= 55 * 1000);
    TEST_ASSERT_TRUE(g_max_hold_us >= 50 * 1000);
    TEST_ASSERT_EQUAL_UINT32(bus.n_ops, g_lock_count); /* exactly one operation per acquisition */
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_NO_SENSOR, accel_select_state(&sel));
}

/* ADV-IMPL-02: sensor reset/standby while reads still succeed on the bus. */
void test_sensor_reset_gives_strikes_then_no_sensor_and_never_a_sample(void)
{
    make_active();
    memset(&bus.regs[0x2C], 0, 13); /* power-on-reset: POWER_CTL 0, DATA_FORMAT 0, data 0 */
    bus.regs[0x2C] = 0x0A;
    int16_t raw[3] = { 77, 77, 77 };
    int64_t t = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_CONFIG, accel_select_read(&sel, raw, &t));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_NO_SENSOR, accel_select_state(&sel)); /* 3rd strike */
    TEST_ASSERT_EQUAL_INT16(77, raw[0]);                                  /* no hardware data was produced */
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL));

    /* the probe task reconfigures the part and the sensor is active again */
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_read(&sel, raw, &t));
}

/* R2-02: the sensor settles (>= 20 ms, lock not held) AFTER the last configuration
 * transaction and BEFORE it becomes ACTIVE, so the first sample is a real conversion. */
void test_settle_delay_after_configuration_before_active(void)
{
    bus.present_addr = 0x53;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_TRUE(g_delay_total_ms >= 20);
    TEST_ASSERT_EQUAL_UINT32(bus.n_ops, g_ops_at_delay); /* no bus operation follows the delay */
    TEST_ASSERT_EQUAL_UINT32(bus.n_ops, g_lock_count);   /* and the delay itself took no lock */
}

void test_no_settle_delay_when_configuration_fails(void)
{
    bus.present_addr = 0x53;
    bus.regs[0x00] = 0x00; /* wrong identity */
    (void)accel_select_probe_cycle(&sel);
    TEST_ASSERT_EQUAL_UINT32(0, g_delay_total_ms);
}

/* R2-01: two wrong parts (0x53 and 0x1D) are each reported once, not once per cycle. */
void test_two_wrong_devices_are_each_reported_once(void)
{
    bus.wrong_addr = 0x53;   /* DEVID 0x00 */
    bus.present_addr = 0x1D; /* ACKs as well, DEVID set to a wrong value */
    bus.regs[0x00] = 0x00;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_IDENTITY, accel_select_probe_cycle(&sel));
    }
    TEST_ASSERT_EQUAL_UINT32(2, count_evt(ACCEL_SEL_EVT_FAIL)); /* one per address, then silent */
}

/* ADV-IMPL-01: a wrong device at 0x53 must not hide a real ADXL345 at 0x1D. */
void test_wrong_device_at_first_address_real_part_at_second_is_found(void)
{
    bus.wrong_addr = 0x53; /* ACKs, DEVID 0x00 */
    bus.present_addr = 0x1D;
    TEST_ASSERT_EQUAL_INT(ESP_OK, accel_select_probe_cycle(&sel));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
    TEST_ASSERT_EQUAL_UINT8(0x1D, accel_select_addr(&sel));
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL)); /* the wrong part, reported once */
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_UP));
    TEST_ASSERT_EQUAL_UINT32(0, bus.wrong_regs[0x2D]); /* nothing was ever written to the wrong part */
}

void test_a_permanently_wrong_device_is_reported_once_per_reason(void)
{
    bus.wrong_addr = 0x53;
    bus.present_addr = 0;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_IDENTITY, accel_select_probe_cycle(&sel));
    }
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL)); /* same (addr, reason): no repeat */
    TEST_ASSERT_EQUAL_UINT32(0, bus.n_writes);
    bus.wrong_addr = 0x1D; /* a different (addr, reason) is a new report */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_ACCEL_IDENTITY, accel_select_probe_cycle(&sel));
}

/* REV-THRD-01 / ADV-IMPL-01: the probe task must not re-arm itself. Worst case: the
 * notify callback "wakes" the task on every event and the task is polled every 1 ms for
 * 30 s. At most one cycle may start per 5 s interval. */
void test_probe_loop_runs_at_most_one_cycle_per_interval_despite_wakes(void)
{
    bus.wrong_addr = 0x53;
    bus.present_addr = 0;
    accel_probe_sched_t ps;
    accel_probe_sched_init(&ps, 5000);
    size_t cycles = 0;
    for (int64_t ms = 0; ms <= 30000; ms++) {
        if (g_wake) {
            g_wake = false; /* the task was woken ... */
        }
        if (accel_probe_step(&sel, &ps, ms * 1000)) { /* ... but spacing still holds */
            cycles++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(7, cycles); /* t = 0, 5, 10, 15, 20, 25, 30 s */
    TEST_ASSERT_TRUE(bus.n_ops <= 7 * 3);
    TEST_ASSERT_EQUAL_UINT32(1, count_evt(ACCEL_SEL_EVT_FAIL));
}

void test_probe_wait_and_step_rules(void)
{
    accel_probe_sched_t ps;
    accel_probe_sched_init(&ps, 5000);
    TEST_ASSERT_EQUAL_UINT32(0, accel_probe_wait_ms(&ps, 123)); /* first cycle at once */
    bus.present_addr = 0;
    TEST_ASSERT_TRUE(accel_probe_step(&sel, &ps, 1000000));
    TEST_ASSERT_EQUAL_UINT32(5000, accel_probe_wait_ms(&ps, 1000000));
    TEST_ASSERT_EQUAL_UINT32(1, accel_probe_wait_ms(&ps, 1000000 + 4999000));
    TEST_ASSERT_FALSE(accel_probe_step(&sel, &ps, 1000000 + 4999000));
    TEST_ASSERT_TRUE(accel_probe_step(&sel, &ps, 1000000 + 5000000));
    /* an ACTIVE sensor is never probed */
    bus.present_addr = 0x53;
    TEST_ASSERT_TRUE(accel_probe_step(&sel, &ps, 100000000));
    TEST_ASSERT_EQUAL_INT(ACCEL_SEL_ACTIVE, accel_select_state(&sel));
    size_t ops = bus.n_ops;
    TEST_ASSERT_FALSE(accel_probe_step(&sel, &ps, 900000000));
    TEST_ASSERT_EQUAL_UINT32(ops, bus.n_ops);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sensor_reset_gives_strikes_then_no_sensor_and_never_a_sample);
    RUN_TEST(test_settle_delay_after_configuration_before_active);
    RUN_TEST(test_no_settle_delay_when_configuration_fails);
    RUN_TEST(test_two_wrong_devices_are_each_reported_once);
    RUN_TEST(test_wrong_device_at_first_address_real_part_at_second_is_found);
    RUN_TEST(test_a_permanently_wrong_device_is_reported_once_per_reason);
    RUN_TEST(test_probe_loop_runs_at_most_one_cycle_per_interval_despite_wakes);
    RUN_TEST(test_probe_wait_and_step_rules);
    RUN_TEST(test_absent_then_present_switches_within_one_cycle);
    RUN_TEST(test_alternate_address_is_found);
    RUN_TEST(test_probe_results_are_logged_on_change_only);
    RUN_TEST(test_wrong_identity_means_no_config_writes_and_no_sensor);
    RUN_TEST(test_read_when_not_active_never_touches_the_bus);
    RUN_TEST(test_successful_read_returns_data_and_acquisition_time);
    RUN_TEST(test_three_failures_drop_to_no_sensor_then_reprobe_recovers);
    RUN_TEST(test_a_success_resets_the_failure_count);
    RUN_TEST(test_lock_timeout_is_a_busy_failure);
    RUN_TEST(test_lock_hold_bound_one_op_per_lock_and_one_recover_per_cycle);
    return UNITY_END();
}
