// Host test for lib/odo_core. Run: sh test/host/run.sh
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "odo_core.h"

static int s_fail, s_run;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++s_run;                                                        \
        if (!(cond)) {                                                  \
            ++s_fail;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

// 1 m north at 52 N, in degrees of latitude
#define M_LAT (1.0 / 111195.0)

// 1 m east at 52 N, in degrees of longitude
#define M_LON (1.0 / (111195.0 * 0.615661))

static odo_fix_t fix(double north_m, float kmh, uint32_t t_ms)
{
    odo_fix_t f = {52.0 + north_m * M_LAT, 21.0, kmh, 1.0f, 8, t_ms};
    return f;
}

static odo_fix_t fix_ne(double north_m, double east_m, float kmh, uint32_t t_ms)
{
    odo_fix_t f = fix(north_m, kmh, t_ms);
    f.lon += east_m * M_LON;
    return f;
}

static void test_distance(void)
{
    NEAR(odo_dist_m(52, 21, 52, 21), 0, 1e-9);
    NEAR(odo_dist_m(52, 21, 52 + 1000 * M_LAT, 21), 1000, 0.5);
    NEAR(odo_dist_m(0, 0, 0, 1), 111195, 1);  // 1 deg of longitude on the equator
}

static void test_straight_drive(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    // 36 km/h = 10 m/s for 60 s
    for (int i = 0; i <= 60; ++i) {
        odo_step(&s, &(odo_fix_t){52.0 + i * 10 * M_LAT, 21.0, 36.0f, 1.0f, 8, 1000u * i});
    }
    NEAR(s.trip_m, 600, 1);
    NEAR(s.total_m, 600, 1);
    CHECK(s.moving_ms == 60000);
    NEAR(s.max_kmh, 36, 1e-6);
}

static void test_standing_jitter_not_counted(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 100; ++i) {  // +-3 m wander, speed below threshold
        odo_step(&s, &(odo_fix_t){52.0 + ((i % 2) ? 3 : -3) * M_LAT, 21.0, 1.2f, 1.0f, 8, 1000u * i});
    }
    NEAR(s.trip_m, 0, 1e-9);
    CHECK(s.moving_ms == 0);
}

static void test_second_update_same_second_ignored(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 36, 0), b = fix(10, 36, 1000), b2 = fix_ne(10, 4, 36, 1020), c = fix(20, 36, 2000);
    odo_step(&s, &a);
    odo_step(&s, &b);
    odo_step(&s, &b2);  // RMC after GGA 20 ms later, 4 m of noise: must not add the zig-zag
    odo_step(&s, &c);
    NEAR(s.trip_m, 20, 0.1);
    CHECK(s.moving_ms == 2000);
}

static void test_bad_fix_breaks_track(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 36, 0), bad = fix_ne(10, 40, 36, 1000), c = fix(20, 36, 2000), d = fix(30, 36, 3000);
    bad.hdop = 9.0f;  // plausible position (41 m in 1 s), only HDOP says it is bad
    odo_step(&s, &a);
    CHECK(!odo_step(&s, &bad));
    odo_step(&s, &c);  // re-anchor
    odo_step(&s, &d);
    NEAR(s.trip_m, 10, 0.1);
    odo_fix_t few = fix(40, 36, 4000);
    few.sats = 3;
    odo_step(&s, &few);
    NEAR(s.trip_m, 10, 0.1);
}

static void test_drift_while_standing_then_move(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i <= 10; ++i) {  // standing, position creeps 2 m per fix
        odo_fix_t f = fix(2.0 * i, 1.0f, 1000u * i);
        odo_step(&s, &f);
    }
    odo_fix_t go = fix(30, 36, 11000);  // then drives 10 m
    odo_step(&s, &go);
    NEAR(s.trip_m, 10, 0.1);
}

static void test_gap_and_jump(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 36, 0), far_later = fix(2000, 36, 60000), next = fix(2010, 36, 61000);
    odo_step(&s, &a);
    odo_step(&s, &far_later);  // 60 s gap (tunnel): no straight line
    odo_step(&s, &next);
    NEAR(s.trip_m, 10, 0.1);

    memset(&s, 0, sizeof s);
    odo_fix_t p = fix(0, 36, 0), jump = fix(400, 36, 1000), q = fix(410, 36, 2000);
    odo_step(&s, &p);
    odo_step(&s, &jump);  // 400 m in 1 s = position glitch
    odo_step(&s, &q);
    NEAR(s.trip_m, 10, 0.1);
}

static void test_max_speed_needs_good_fix(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 50, 0);
    odo_step(&s, &a);
    odo_fix_t spike = fix(14, 180, 1000);
    spike.hdop = 3.5f;  // usable for distance, not for max
    odo_step(&s, &spike);
    NEAR(s.max_kmh, 50, 1e-6);
    odo_fix_t silly = fix(28, 900, 2000);
    odo_step(&s, &silly);
    NEAR(s.max_kmh, 50, 1e-6);
}

static void test_reset_trip_keeps_total(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 36, 0), b = fix(10, 36, 1000), c = fix(20, 36, 2000);
    odo_step(&s, &a);
    odo_step(&s, &b);
    odo_reset_trip(&s);
    NEAR(s.trip_m, 0, 0);
    CHECK(s.max_kmh == 0 && s.moving_ms == 0);
    NEAR(s.total_m, 10, 0.1);
    odo_step(&s, &c);  // first fix after reset only anchors
    NEAR(s.trip_m, 0, 0);
    NEAR(s.total_m, 10, 0.1);
}

static void test_millis_wrap(void)
{
    odo_state_t s;
    memset(&s, 0, sizeof s);
    odo_fix_t a = fix(0, 36, 0xFFFFFC18u), b = fix(10, 36, 0xFFFFFC18u + 1000u);  // b.t_ms wraps to 0
    odo_step(&s, &a);
    odo_step(&s, &b);
    NEAR(s.trip_m, 10, 0.1);
    CHECK(s.moving_ms == 1000);
}

int main(void)
{
    test_distance();
    test_straight_drive();
    test_standing_jitter_not_counted();
    test_second_update_same_second_ignored();
    test_bad_fix_breaks_track();
    test_drift_while_standing_then_move();
    test_gap_and_jump();
    test_max_speed_needs_good_fix();
    test_reset_trip_keeps_total();
    test_millis_wrap();
    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
