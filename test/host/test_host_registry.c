/* TC-UT-04/05: registration table, dispatch order, generation counter incl. wrap,
 * engagement and link release, security deadline decision, pending bits
 * (REQ-FUNC-64, 71, 72, 76; tech_spec.md 2.3). */
#include <string.h>

#include "unity.h"
#include "host_registry.h"

void setUp(void) {}
void tearDown(void) {}

static int svc_a, svc_b, svc_c;
static const uint8_t uuid[16] = { 1 };

static int g_order[8];
static int g_n;
static uint32_t g_gen_seen[8];

static void cb_a(const ble_host_evt_t *e, void *u)
{
    (void)u;
    g_order[g_n] = 0;
    g_gen_seen[g_n++] = e->gen;
}

static void cb_b(const ble_host_evt_t *e, void *u)
{
    (void)u;
    g_order[g_n] = 1;
    g_gen_seen[g_n++] = e->gen;
}

static ble_host_client_cfg_t cfg(const char *name, const void *svcs, bool dflt, ble_host_evt_cb_t cb)
{
    ble_host_client_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.name = name;
    c.svcs = (const struct ble_gatt_svc_def *)svcs;
    c.adv_uuid128 = uuid;
    c.default_primary = dflt;
    c.adv_policy = dflt ? BLE_HOST_ADV_ALWAYS : BLE_HOST_ADV_WHEN_PRIMARY;
    c.cb = cb;
    return c;
}

void test_registration_rules_and_no_change_on_error(void)
{
    host_reg_t r;
    ble_host_client_id_t id = 99;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, cb_a);
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, host_reg_register(&r, false, &a, &id)); /* after start */
    TEST_ASSERT_EQUAL_UINT8(0, host_reg_count(&r));
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_reg_register(&r, true, &a, &id));
    TEST_ASSERT_EQUAL_UINT8(0, id);
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_reg_register(&r, true, &b, &id));
    TEST_ASSERT_EQUAL_UINT8(1, id);

    ble_host_client_cfg_t dup_name = cfg("ota", &svc_c, false, cb_a);
    ble_host_client_cfg_t dup_svcs = cfg("x", &svc_a, false, cb_a);
    ble_host_client_cfg_t third = cfg("third", &svc_c, false, cb_a);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_reg_register(&r, true, &dup_name, &id));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_reg_register(&r, true, &dup_svcs, &id));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM, host_reg_register(&r, true, &third, &id)); /* over capacity */
    TEST_ASSERT_EQUAL_UINT8(2, host_reg_count(&r));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_reg_register(&r, true, NULL, &id));
}

void test_second_default_primary_is_rejected(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 4);
    ble_host_client_cfg_t a = cfg("cli", &svc_a, true, cb_a);
    ble_host_client_cfg_t b = cfg("other", &svc_b, true, cb_b);
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_reg_register(&r, true, &a, &id));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_reg_register(&r, true, &b, &id));
    TEST_ASSERT_EQUAL_UINT8(1, host_reg_count(&r));
}

/* Invariant I-1: the add_svcs order equals the registration order. */
void test_svc_order_equals_registration_order(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, cb_a);
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);
    host_reg_register(&r, true, &a, &id);
    host_reg_register(&r, true, &b, &id);
    TEST_ASSERT_TRUE((const void *)host_reg_get(&r, 0)->svcs == (const void *)&svc_a);
    TEST_ASSERT_TRUE((const void *)host_reg_get(&r, 1)->svcs == (const void *)&svc_b);
    TEST_ASSERT_NULL(host_reg_get(&r, 2));
}

void test_dispatch_in_order_with_same_gen(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, cb_a);
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);
    host_reg_register(&r, true, &a, &id);
    host_reg_register(&r, true, &b, &id);
    ble_host_evt_t e;
    memset(&e, 0, sizeof(e));
    e.gen = host_reg_next_gen(&r);
    g_n = 0;
    host_reg_dispatch(&r, &e);
    TEST_ASSERT_EQUAL_INT(2, g_n);
    TEST_ASSERT_EQUAL_INT(0, g_order[0]);
    TEST_ASSERT_EQUAL_INT(1, g_order[1]);
    TEST_ASSERT_EQUAL_UINT32(g_gen_seen[0], g_gen_seen[1]);
}

void test_primary_defaults_to_the_default_client(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, cb_a);
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);
    host_reg_register(&r, true, &a, &id);
    host_reg_register(&r, true, &b, &id);
    TEST_ASSERT_EQUAL_INT(1, host_reg_primary(&r));
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_reg_set_primary(&r, 0, true));
    TEST_ASSERT_EQUAL_INT(0, host_reg_primary(&r));
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_reg_set_primary(&r, 0, false));
    TEST_ASSERT_EQUAL_INT(1, host_reg_primary(&r));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_reg_set_primary(&r, 7, true));
}

void test_generation_increments_and_skips_zero_on_wrap(void)
{
    host_reg_t r;
    host_reg_init(&r, 2);
    TEST_ASSERT_EQUAL_UINT32(1, host_reg_gen(&r));
    TEST_ASSERT_EQUAL_UINT32(2, host_reg_next_gen(&r));
    TEST_ASSERT_EQUAL_UINT32(3, host_reg_next_gen(&r));
    r.gen = UINT32_MAX;
    TEST_ASSERT_EQUAL_UINT32(1, host_reg_next_gen(&r)); /* 0 is the "no link" sentinel */
}

/* A Service Changed subscribe or an OTA TX restore never engages the CLI; an
 * OTA-only link is released by stop. */
void test_engagement_and_release_policy(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, cb_a);
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);
    host_reg_register(&r, true, &a, &id);
    host_reg_register(&r, true, &b, &id);
    uint32_t gen = host_reg_next_gen(&r);
    host_reg_clear_engaged(&r);

    /* (c) neither engaged: kept */
    TEST_ASSERT_FALSE(host_reg_release_allowed(&r, 0, gen, true));
    /* (b) OTA only: terminated */
    host_reg_note_engaged(&r, 0, gen);
    TEST_ASSERT_TRUE(host_reg_release_allowed(&r, 0, gen, true));
    TEST_ASSERT_FALSE(host_reg_release_allowed(&r, 0, gen, false)); /* no link */
    TEST_ASSERT_FALSE(host_reg_release_allowed(&r, 0, gen + 1, true)); /* stale generation */
    /* (a) CLI engaged as well: kept */
    host_reg_note_engaged(&r, 1, gen);
    TEST_ASSERT_FALSE(host_reg_release_allowed(&r, 0, gen, true));
    /* engagement with a stale generation is ignored */
    host_reg_clear_engaged(&r);
    host_reg_note_engaged(&r, 1, gen - 1);
    TEST_ASSERT_EQUAL_UINT32(0, r.engaged_mask);
}

void test_security_deadline_decision(void)
{
    TEST_ASSERT_TRUE(host_deadline_should_terminate(true, 5, 5, false));   /* expiry on an insecure link */
    TEST_ASSERT_FALSE(host_deadline_should_terminate(true, 5, 5, true));   /* became secure */
    TEST_ASSERT_FALSE(host_deadline_should_terminate(false, 5, 5, false)); /* disconnected */
    TEST_ASSERT_FALSE(host_deadline_should_terminate(true, 6, 5, false));  /* old generation */
    TEST_ASSERT_FALSE(host_deadline_should_terminate(true, 0, 0, false));  /* the "no link" sentinel */
}

void test_pending_bits_merge_and_are_taken_once(void)
{
    _Atomic uint32_t w = 0;
    host_pending_set(&w, HOST_PEND_ADV_RETRY);
    host_pending_set(&w, HOST_PEND_SEC_DEADLINE); /* two bits set before one kick is handled */
    uint32_t bits = host_pending_take(&w);
    TEST_ASSERT_EQUAL_UINT32(HOST_PEND_ADV_RETRY | HOST_PEND_SEC_DEADLINE, bits);
    TEST_ASSERT_EQUAL_UINT32(0, host_pending_take(&w));
    host_pending_set(&w, HOST_PEND_SEC_RETRY); /* a bit set after the take is seen by the next kick */
    TEST_ASSERT_EQUAL_UINT32(HOST_PEND_SEC_RETRY, host_pending_take(&w));
}

static int64_t fake_now;
static int64_t now_fn(void)
{
    fake_now += 1000;
    return fake_now;
}

static void slow_cb(const ble_host_evt_t *e, void *u)
{
    (void)e;
    (void)u;
    fake_now += 7000;
}

void test_timed_dispatch_excludes_waived_clients(void)
{
    host_reg_t r;
    ble_host_client_id_t id;
    host_reg_init(&r, 2);
    ble_host_client_cfg_t a = cfg("ota", &svc_a, false, slow_cb);
    a.waive_timing = true;
    ble_host_client_cfg_t b = cfg("cli", &svc_b, true, cb_b);
    host_reg_register(&r, true, &a, &id);
    host_reg_register(&r, true, &b, &id);
    ble_host_evt_t e;
    memset(&e, 0, sizeof(e));
    uint32_t max_us = 0;
    g_n = 0;
    host_reg_dispatch_timed(&r, &e, now_fn, &max_us);
    TEST_ASSERT_TRUE(max_us > 0);
    TEST_ASSERT_TRUE(max_us < 7000); /* the waived 7 ms callback is not counted */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registration_rules_and_no_change_on_error);
    RUN_TEST(test_second_default_primary_is_rejected);
    RUN_TEST(test_svc_order_equals_registration_order);
    RUN_TEST(test_dispatch_in_order_with_same_gen);
    RUN_TEST(test_primary_defaults_to_the_default_client);
    RUN_TEST(test_generation_increments_and_skips_zero_on_wrap);
    RUN_TEST(test_engagement_and_release_policy);
    RUN_TEST(test_security_deadline_decision);
    RUN_TEST(test_pending_bits_merge_and_are_taken_once);
    RUN_TEST(test_timed_dispatch_excludes_waived_clients);
    return UNITY_END();
}
