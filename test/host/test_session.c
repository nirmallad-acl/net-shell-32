/* TC-UT-10 / TC-UT-11 (session part): the lock-free claim, publish and cancel of the
 * system-wide accel session (REQ-FUNC-47, 49; tech_spec.md 2.6.5). */
#include "unity.h"
#include "accel_session_core.h"

void setUp(void) {}
void tearDown(void) {}

static accel_session_core_t c;

static uint32_t start(diag_transport_t t, uint32_t gen)
{
    TEST_ASSERT_TRUE(accel_core_try_claim(&c));
    accel_core_set_owner(&c, t, gen);
    return accel_core_publish(&c);
}

void test_single_system_wide_session(void)
{
    accel_core_init(&c);
    uint32_t sid = start(DIAG_TRANSPORT_BLE, 3);
    TEST_ASSERT_TRUE(sid != 0u);
    TEST_ASSERT_FALSE(accel_core_try_claim(&c)); /* a second request gets ERR busy and disturbs nothing */
    TEST_ASSERT_EQUAL_UINT32(sid, accel_core_active_sid(&c));
    accel_core_release(&c);
    TEST_ASSERT_EQUAL_UINT32(0, accel_core_active_sid(&c));
    TEST_ASSERT_TRUE(accel_core_try_claim(&c));
}

void test_session_ids_are_monotonic_and_never_zero(void)
{
    accel_core_init(&c);
    uint32_t s1 = start(DIAG_TRANSPORT_SERIAL, 0);
    accel_core_release(&c);
    uint32_t s2 = start(DIAG_TRANSPORT_SERIAL, 0);
    accel_core_release(&c);
    TEST_ASSERT_TRUE(s2 == s1 + 1u);
    c.sid_counter = UINT32_MAX;
    TEST_ASSERT_TRUE(accel_core_try_claim(&c));
    TEST_ASSERT_EQUAL_UINT32(1, accel_core_publish(&c)); /* wraps past 0 */
}

void test_cancel_matches_transport_and_generation(void)
{
    accel_core_init(&c);
    uint32_t sid = start(DIAG_TRANSPORT_BLE, 7);
    uint32_t why = 99;

    TEST_ASSERT_FALSE(accel_core_cancel(&c, DIAG_TRANSPORT_SERIAL, 0, DIAG_CANCEL_DISCONNECT)); /* other transport */
    TEST_ASSERT_FALSE(accel_core_cancel(&c, DIAG_TRANSPORT_BLE, 8, DIAG_CANCEL_DISCONNECT));    /* other generation */
    TEST_ASSERT_FALSE(accel_core_cancelled(&c, sid, &why));

    TEST_ASSERT_TRUE(accel_core_cancel(&c, DIAG_TRANSPORT_BLE, 7, DIAG_CANCEL_DISCONNECT));
    TEST_ASSERT_TRUE(accel_core_cancelled(&c, sid, &why));
    TEST_ASSERT_EQUAL_UINT32(DIAG_CANCEL_DISCONNECT, why);
    accel_core_release(&c);
}

void test_serial_session_ignores_the_generation_argument(void)
{
    accel_core_init(&c);
    uint32_t sid = start(DIAG_TRANSPORT_SERIAL, 0);
    TEST_ASSERT_TRUE(accel_core_cancel(&c, DIAG_TRANSPORT_SERIAL, 12345, DIAG_CANCEL_TIMEOUT));
    uint32_t why = 0;
    TEST_ASSERT_TRUE(accel_core_cancelled(&c, sid, &why));
    TEST_ASSERT_EQUAL_UINT32(DIAG_CANCEL_TIMEOUT, why);
}

void test_reboot_with_transport_any_cancels_any_session(void)
{
    accel_core_init(&c);
    uint32_t sid = start(DIAG_TRANSPORT_BLE, 4);
    TEST_ASSERT_TRUE(accel_core_cancel(&c, DIAG_TRANSPORT_ANY, 0, DIAG_CANCEL_REBOOT));
    uint32_t why = 0;
    TEST_ASSERT_TRUE(accel_core_cancelled(&c, sid, &why));
    TEST_ASSERT_EQUAL_UINT32(DIAG_CANCEL_REBOOT, why);
    accel_core_release(&c);
    sid = start(DIAG_TRANSPORT_SERIAL, 0);
    TEST_ASSERT_TRUE(accel_core_cancel(&c, DIAG_TRANSPORT_ANY, 0, DIAG_CANCEL_REBOOT));
    TEST_ASSERT_TRUE(accel_core_cancelled(&c, sid, &why));
}

void test_cancel_without_a_session_is_a_no_op(void)
{
    accel_core_init(&c);
    TEST_ASSERT_FALSE(accel_core_cancel(&c, DIAG_TRANSPORT_ANY, 0, DIAG_CANCEL_REBOOT));
    TEST_ASSERT_FALSE(accel_core_cancelled(&c, 1, NULL));
    /* claimed but not yet published: nothing to cancel either */
    TEST_ASSERT_TRUE(accel_core_try_claim(&c));
    TEST_ASSERT_FALSE(accel_core_cancel(&c, DIAG_TRANSPORT_ANY, 0, DIAG_CANCEL_REBOOT));
    accel_core_abort_unpublished(&c);
    TEST_ASSERT_TRUE(accel_core_try_claim(&c)); /* the claim was released again */
}

/* A cancel that raced with the end of one session and the start of the next names the
 * old session id and is ignored by the new one. */
void test_late_cancel_cannot_hit_a_newer_session(void)
{
    accel_core_init(&c);
    uint32_t old_sid = start(DIAG_TRANSPORT_BLE, 7);
    TEST_ASSERT_TRUE(accel_core_cancel(&c, DIAG_TRANSPORT_BLE, 7, DIAG_CANCEL_DISCONNECT));
    accel_core_release(&c); /* the old session ended */
    uint32_t new_sid = start(DIAG_TRANSPORT_BLE, 8);
    TEST_ASSERT_TRUE(new_sid != old_sid);
    uint32_t why = 0;
    TEST_ASSERT_FALSE(accel_core_cancelled(&c, new_sid, &why)); /* the stale cancel names old_sid */
    TEST_ASSERT_TRUE(accel_core_cancelled(&c, old_sid, &why));  /* and only matches the old one */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_system_wide_session);
    RUN_TEST(test_session_ids_are_monotonic_and_never_zero);
    RUN_TEST(test_cancel_matches_transport_and_generation);
    RUN_TEST(test_serial_session_ignores_the_generation_argument);
    RUN_TEST(test_reboot_with_transport_any_cancels_any_session);
    RUN_TEST(test_cancel_without_a_session_is_a_no_op);
    RUN_TEST(test_late_cancel_cannot_hit_a_newer_session);
    return UNITY_END();
}
