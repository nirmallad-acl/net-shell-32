/* TC-UT-13b: seedable simulator (REQ-FUNC-54, 55). */
#include "unity.h"
#include "accel_sim.h"

void setUp(void) {}
void tearDown(void) {}

static const accel_sim_range_t range = { .xy_min_mg = -250, .xy_max_mg = 250, .z_min_mg = 750, .z_max_mg = 1250 };

void test_same_seed_gives_identical_stream(void)
{
    accel_sim_t a;
    accel_sim_t b;
    accel_sim_seed(&a, 12345);
    accel_sim_seed(&b, 12345);
    for (int i = 0; i < 100; i++) {
        int32_t ax, ay, az, bx, by, bz;
        accel_sim_next_mg(&a, &range, &ax, &ay, &az);
        accel_sim_next_mg(&b, &range, &bx, &by, &bz);
        TEST_ASSERT_EQUAL_INT32(ax, bx);
        TEST_ASSERT_EQUAL_INT32(ay, by);
        TEST_ASSERT_EQUAL_INT32(az, bz);
    }
}

void test_different_seeds_differ_and_the_stream_continues(void)
{
    accel_sim_t a;
    accel_sim_t b;
    accel_sim_seed(&a, 1);
    accel_sim_seed(&b, 2);
    int differ = 0;
    int32_t px = 0, py = 0, pz = 0;
    int moved = 0;
    for (int i = 0; i < 15; i++) {
        int32_t ax, ay, az, bx, by, bz;
        accel_sim_next_mg(&a, &range, &ax, &ay, &az);
        accel_sim_next_mg(&b, &range, &bx, &by, &bz);
        if (ax != bx || ay != by || az != bz) {
            differ++;
        }
        if (i > 0 && (ax != px || ay != py || az != pz)) {
            moved++; /* consecutive samples (and so consecutive sessions) are not identical */
        }
        px = ax;
        py = ay;
        pz = az;
    }
    TEST_ASSERT_TRUE(differ > 10);
    TEST_ASSERT_TRUE(moved > 10);
}

void test_ten_thousand_draws_stay_in_range(void)
{
    accel_sim_t s;
    accel_sim_seed(&s, 0xC0FFEEu);
    int32_t minx = 9999, maxx = -9999, minz = 9999, maxz = -9999;
    for (int i = 0; i < 10000; i++) {
        int32_t x, y, z;
        accel_sim_next_mg(&s, &range, &x, &y, &z);
        TEST_ASSERT_TRUE(x >= -250 && x <= 250);
        TEST_ASSERT_TRUE(y >= -250 && y <= 250);
        TEST_ASSERT_TRUE(z >= 750 && z <= 1250);
        if (x < minx) { minx = x; }
        if (x > maxx) { maxx = x; }
        if (z < minz) { minz = z; }
        if (z > maxz) { maxz = z; }
    }
    TEST_ASSERT_TRUE(minx < -200 && maxx > 200); /* the range is actually used */
    TEST_ASSERT_TRUE(minz < 800 && maxz > 1200);
}

void test_zero_seed_never_sticks_the_generator(void)
{
    accel_sim_t s;
    accel_sim_seed(&s, 0);
    TEST_ASSERT_TRUE(s.state != 0u);
    uint32_t first = accel_sim_rand(&s);
    TEST_ASSERT_TRUE(first != 0u);
    TEST_ASSERT_TRUE(accel_sim_rand(&s) != first);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_same_seed_gives_identical_stream);
    RUN_TEST(test_different_seeds_differ_and_the_stream_continues);
    RUN_TEST(test_ten_thousand_draws_stay_in_range);
    RUN_TEST(test_zero_seed_never_sticks_the_generator);
    return UNITY_END();
}
