/* TC-UT-05: the ble_host_exec() state machine (tech_spec.md 2.3.5, R3-02, ADV-CONC-12)
 * against a mock clock, mock mutex/semaphore and a mock event queue. The mock
 * "host task" is driven explicitly by the test: it can run the event early, late
 * or never. */
#include <string.h>

#include "unity.h"
#include "host_exec.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- mock environment ---- */
static int64_t g_now_us;
static bool    g_mtx_busy;          /* another caller holds s_exec_mtx */
static int     g_sem_count;         /* binary semaphore */
static bool    g_event_queued;      /* de-duplicated single static event */
static bool    g_host_up;
static bool    g_on_host_task;
static int     g_posts;
static int     g_stuck_logs;
static uint32_t g_stuck_seq;
static host_exec_t g_x;
static uint32_t g_early_wake_ms; /* if non-zero, the next wait is woken after this many ms */

/* Scripted behaviour while the caller waits: called from sem_take with the time
 * that is about to elapse. The test installs a hook that runs the executor. */
typedef void (*wait_hook_t)(int64_t t_before_us, int64_t t_after_us);
static wait_hook_t g_hook;

static int64_t m_now(void *c) { (void)c; return g_now_us; }

static bool m_mtx_take(void *c, uint32_t ms)
{
    (void)c;
    if (g_mtx_busy) {
        g_now_us += (int64_t)ms * 1000; /* waits the whole time and fails */
        return false;
    }
    return true;
}

static void m_mtx_give(void *c) { (void)c; }

static bool m_sem_take(void *c, uint32_t ms)
{
    (void)c;
    if (g_sem_count > 0) {
        g_sem_count--;
        return true;
    }
    int64_t before = g_now_us;
    uint32_t adv = ms; /* nothing happens until the timeout elapses (or an early wake) ... */
    if (ms > 0) { /* the caller's non-blocking drain take (ms == 0) must not consume the one-shot */
        if (g_early_wake_ms > 0 && g_early_wake_ms < ms) {
            adv = g_early_wake_ms;
        }
        g_early_wake_ms = 0; /* one-shot */
    }
    g_now_us += (int64_t)adv * 1000;
    if (g_hook != NULL && ms > 0) {
        g_hook(before, g_now_us);   /* ... except what the script says */
    }
    if (g_sem_count > 0) {
        g_sem_count--;
        return true;
    }
    return false;
}

static void m_sem_give(void *c) { (void)c; if (g_sem_count < 1) { g_sem_count = 1; } }

static bool m_post(void *c)
{
    (void)c;
    g_posts++;
    g_event_queued = true;
    return true;
}

static bool m_host_up(void *c) { (void)c; return g_host_up; }
static bool m_on_host(void *c) { (void)c; return g_on_host_task; }
static void m_stuck(void *c, uint32_t seq) { (void)c; g_stuck_logs++; g_stuck_seq = seq; }

static const host_exec_ops_t ops = {
    .ctx = NULL, .now_us = m_now, .mtx_take = m_mtx_take, .mtx_give = m_mtx_give, .sem_take = m_sem_take,
    .sem_give = m_sem_give, .post = m_post, .host_up = m_host_up, .on_host_task = m_on_host, .log_stuck = m_stuck,
};

static int g_fn_calls;
static int32_t fn_ok(uintptr_t arg) { g_fn_calls++; return (int32_t)(arg + 100); }

static void reset(void)
{
    g_now_us = 1000000;
    g_mtx_busy = false;
    g_sem_count = 0;
    g_event_queued = false;
    g_host_up = true;
    g_on_host_task = false;
    g_posts = 0;
    g_stuck_logs = 0;
    g_hook = NULL;
    g_early_wake_ms = 0;
    g_fn_calls = 0;
    host_exec_init(&g_x);
}

/* The host task runs the queued event (if any). */
static void host_runs(void)
{
    if (g_event_queued) {
        g_event_queued = false;
        host_exec_run(&g_x, &ops);
    }
}

static void hook_run_immediately(int64_t b, int64_t a) { (void)b; (void)a; host_runs(); }

void test_normal_completion_returns_fn_result_by_value(void)
{
    reset();
    g_hook = hook_run_immediately;
    int32_t res = -1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 5, 1000, &res));
    TEST_ASSERT_EQUAL_INT32(105, res);
    TEST_ASSERT_EQUAL_INT(1, g_fn_calls);
}

void test_distinct_codes_for_not_up_wrong_task_and_bad_args(void)
{
    reset();
    g_host_up = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_HOST_NOT_UP, host_exec_call(&g_x, &ops, fn_ok, 0, 1000, NULL));
    g_host_up = true;
    g_on_host_task = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_HOST_WRONG_TASK, host_exec_call(&g_x, &ops, fn_ok, 0, 1000, NULL));
    g_on_host_task = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_exec_call(&g_x, &ops, NULL, 0, 1000, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, host_exec_call(&g_x, &ops, fn_ok, 0, 350, NULL)); /* <= T_COMPLETE */
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
    TEST_ASSERT_EQUAL_INT(0, g_posts);
}

void test_host_never_runs_gives_timeout_and_fn_never_runs(void)
{
    reset();
    int32_t res = -7;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, &res));
    TEST_ASSERT_EQUAL_INT32(-7, res); /* never written */
    TEST_ASSERT_EQUAL_UINT32(HOST_EXEC_ABANDONED, g_x.state);
    host_runs(); /* the abandoned fn must never run, even if its event is still queued */
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
}

/* An ABANDONED earlier request left its event queued: the queued event runs the
 * NEXT request, and never the abandoned fn. */
void test_queued_abandoned_event_runs_the_next_request(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, NULL)); /* abandoned, event stays queued */
    TEST_ASSERT_TRUE(g_event_queued);
    int posts_before = g_posts;
    int32_t res = 0;
    g_hook = hook_run_immediately;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 2, 1000, &res));
    TEST_ASSERT_EQUAL_INT32(102, res);
    TEST_ASSERT_EQUAL_INT(1, g_fn_calls);
    TEST_ASSERT_TRUE(g_posts == posts_before + 1); /* the put is a (de-duplicated) no-op in the real port */
}

/* R3-02(a): a stale give from request N wakes caller N+1, which keeps waiting and
 * returns its own result, never N's. */
/* A stale give (request N's delayed give) slips in AFTER the caller's drain and
 * BEFORE the host ran the request: the first wake is spurious (state PENDING), the
 * loop keeps waiting within the same deadline and the real completion is returned. */
static void hook_stale_give_then_run_late(int64_t b, int64_t a)
{
    (void)b;
    (void)a;
    g_sem_count = 1;               /* the stale give */
    g_hook = hook_run_immediately; /* the host really runs the request at the next wait */
}

void test_stale_give_does_not_complete_the_next_request(void)
{
    reset();
    g_hook = hook_run_immediately;
    int32_t r1 = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, &r1));
    TEST_ASSERT_EQUAL_INT32(101, r1);

    g_early_wake_ms = 10;
    g_hook = hook_stale_give_then_run_late;
    int32_t r2 = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 2, 1000, &r2));
    TEST_ASSERT_EQUAL_INT32(102, r2); /* its own result, never request 1's */
    TEST_ASSERT_EQUAL_INT(2, g_fn_calls);
}

static void hook_spurious_only(int64_t b, int64_t a)
{
    (void)b;
    (void)a;
    g_sem_count = 1; /* a wake with nothing done */
    g_hook = NULL;
}

void test_spurious_wake_with_pending_state_never_returns_ok(void)
{
    reset();
    g_early_wake_ms = 10;
    g_hook = hook_spurious_only;
    int32_t res = -9;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, &res));
    TEST_ASSERT_EQUAL_INT32(-9, res);
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
}

/* R3-02(b): s_exec_mtx held by another caller for 900 ms of a 1000 ms budget:
 * the call returns within the budget (plus one tick) with TIMEOUT. */
void test_contended_mutex_stays_within_one_deadline(void)
{
    reset();
    g_mtx_busy = true;
    int64_t t0 = g_now_us;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, NULL));
    TEST_ASSERT_TRUE(g_now_us - t0 <= 1000 * 1000 + 10000);
    TEST_ASSERT_EQUAL_INT(0, g_posts);
}

/* The fn started but does not finish in the window: INDETERMINATE, result not
 * written, and a late finish only writes the static record. */
static bool g_started_slowly;
static int32_t slow_fn(uintptr_t arg) { (void)arg; g_started_slowly = true; return 77; }

static void hook_start_but_never_finish(int64_t b, int64_t a)
{
    (void)b;
    (void)a;
    if (g_event_queued) {
        /* the host claims the request (RUNNING) but the fn is "preempted": emulate by
         * flipping the state manually without running it. */
        g_event_queued = false;
        atomic_store(&g_x.state, (uint32_t)HOST_EXEC_RUNNING);
    }
}

void test_indeterminate_and_late_finish_never_corrupts_the_next_call(void)
{
    reset();
    g_started_slowly = false;
    g_hook = hook_start_but_never_finish;
    int32_t res = -5;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_BLE_HOST_EXEC_INDETERMINATE, host_exec_call(&g_x, &ops, slow_fn, 0, 1000, &res));
    TEST_ASSERT_EQUAL_INT32(-5, res);
    TEST_ASSERT_EQUAL_INT(1, g_stuck_logs);
    TEST_ASSERT_EQUAL_UINT32(g_x.seq, g_stuck_seq);

    /* The late fn finishes: it writes the static record only. */
    uint32_t s = g_x.seq;
    g_x.result = 77;
    atomic_store(&g_x.done_seq, s);
    atomic_store(&g_x.state, (uint32_t)HOST_EXEC_DONE);

    /* The next caller never sees the old result. */
    g_hook = hook_run_immediately;
    int32_t r2 = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 9, 1000, &r2));
    TEST_ASSERT_EQUAL_INT32(109, r2);
}

/* R3-05: host-task code never posts. host_exec_run (the executor) must make no
 * post of its own. */
void test_executor_never_posts(void)
{
    reset();
    g_hook = hook_run_immediately;
    int32_t res = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, host_exec_call(&g_x, &ops, fn_ok, 1, 1000, &res));
    TEST_ASSERT_EQUAL_INT(1, g_posts); /* exactly the caller's single post */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_completion_returns_fn_result_by_value);
    RUN_TEST(test_distinct_codes_for_not_up_wrong_task_and_bad_args);
    RUN_TEST(test_host_never_runs_gives_timeout_and_fn_never_runs);
    RUN_TEST(test_queued_abandoned_event_runs_the_next_request);
    RUN_TEST(test_stale_give_does_not_complete_the_next_request);
    RUN_TEST(test_spurious_wake_with_pending_state_never_returns_ok);
    RUN_TEST(test_contended_mutex_stays_within_one_deadline);
    RUN_TEST(test_indeterminate_and_late_finish_never_corrupts_the_next_call);
    RUN_TEST(test_executor_never_posts);
    return UNITY_END();
}
