// Host test for lib/accel_core. Run: sh test/host/run.sh
#include <math.h>
#include <stdio.h>
#include "accel_core.h"
#include "acc_hist.h"
#include <string.h>

static int s_fail, s_run;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++s_run;                                                        \
        if (!(cond)) {                                                  \
            ++s_fail;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

// Stand 2 s (with Doppler noise), then constant acceleration a (m/s^2) from t_go (ms) until v_end,
// sampled every period_ms starting at phase_ms. Returns the finished run.
static acc_result_t drive(double a, uint32_t t_go, float v_end_kmh, uint32_t period_ms, uint32_t phase_ms,
                          float rate_hz, acc_state_t *st)
{
    acc_init(st);
    bool done = false;
    for (uint32_t t = phase_ms; t < 120000 && !done; t += period_ms) {
        double v = t > t_go ? a * (t - t_go) / 1000.0 * 3.6 : ((t / period_ms) % 3) * 0.4;  // noise 0-0.8
        if (v > v_end_kmh) {
            v = v_end_kmh - (v - v_end_kmh) * 3;  // lift off after the top speed
        }
        done = acc_step(st, t, (float)v, true, rate_hz);
    }
    return st->res;
}

// exact values for constant acceleration a from rest
static double t_speed(double a, double kmh) { return kmh / 3.6 / a; }
static double t_dist(double a, double m) { return sqrt(2 * m / a); }

static void test_constant_acceleration_10hz(void)
{
    acc_state_t st;
    const double a = 7.0;  // 0-100 in 3.97 s
    const acc_result_t r = drive(a, 3037, 300, 100, 0, 10, &st);
    CHECK(st.phase == ACC_DONE && !r.aborted && !r.low_rate);
    CHECK(NEAR(r.t[ACC_T_0_50], t_speed(a, 50), 0.03));
    CHECK(NEAR(r.t[ACC_T_0_100], t_speed(a, 100), 0.03));
    CHECK(NEAR(r.t[ACC_T_0_200], t_speed(a, 200), 0.03));
    CHECK(!r.grade_valid);  // no altitude given
    CHECK(NEAR(r.t[ACC_T_100_200], t_speed(a, 200) - t_speed(a, 100), 0.02));
    CHECK(NEAR(r.t[ACC_T_100M], t_dist(a, 100), 0.03));
    CHECK(NEAR(r.t[ACC_T_QUARTER], t_dist(a, 402.336), 0.03));
    CHECK(NEAR(r.v_100m, a * t_dist(a, 100) * 3.6, 0.5));
    CHECK(NEAR(r.v_quarter, a * t_dist(a, 402.336) * 3.6, 0.5));
}

static void test_start_not_on_a_sample(void)
{
    // the car leaves at 3.050 s, between samples: the clock must still start there (+-0.03 s)
    acc_state_t st;
    for (uint32_t go = 3000; go < 3100; go += 17) {
        const acc_result_t r = drive(5.0, go, 120, 100, 0, 10, &st);
        CHECK(NEAR(r.t[ACC_T_0_100], t_speed(5.0, 100), 0.03));
    }
}

static void test_1hz_is_flagged_and_still_close(void)
{
    acc_state_t st;
    const acc_result_t r = drive(4.0, 3400, 300, 1000, 0, 1, &st);
    CHECK(r.low_rate);
    CHECK(NEAR(r.t[ACC_T_0_100], t_speed(4.0, 100), 0.25));
    // speed is linear within a step, so distance crossings are exact even 1 s apart
    CHECK(NEAR(r.t[ACC_T_100M], t_dist(4.0, 100), 0.005));
    CHECK(NEAR(r.t[ACC_T_QUARTER], t_dist(4.0, 402.336), 0.005));
}

static void test_lift_off_ends_the_run(void)
{
    acc_state_t st;
    const acc_result_t r = drive(6.0, 3000, 130, 100, 0, 10, &st);  // tops out at 130, then lifts
    CHECK(st.phase == ACC_DONE);
    CHECK(r.t[ACC_T_0_100] > 0 && r.t[ACC_T_0_200] < 0 && r.t[ACC_T_100_200] < 0);
    CHECK(NEAR(r.v_max, 130, 1));
    CHECK(r.duration_s < t_speed(6.0, 130) + 0.5);  // ends right after lifting, not when stopped
}

static void test_must_stand_before_a_run(void)
{
    acc_state_t st;
    acc_init(&st);
    // already rolling at 20 km/h, accelerating: no run
    for (uint32_t t = 0; t < 20000; t += 100) {
        acc_step(&st, t, 20 + t / 200.0f, true, 10);
    }
    CHECK(st.phase == ACC_IDLE);
    // stand 0.5 s only, then go: still no run
    acc_init(&st);
    for (uint32_t t = 0; t < 500; t += 100) acc_step(&st, t, 0, true, 10);
    acc_step(&st, 600, 5, true, 10);
    CHECK(st.phase != ACC_RUNNING);
    // stand 1 s: armed, creeping at 2.5 km/h keeps it armed
    acc_init(&st);
    for (uint32_t t = 0; t <= 1100; t += 100) acc_step(&st, t, 0.5f, true, 10);
    CHECK(st.phase == ACC_ARMED);
    acc_step(&st, 1200, 2.5f, true, 10);
    CHECK(st.phase == ACC_ARMED);
    acc_step(&st, 1300, 4.0f, true, 10);
    CHECK(st.phase == ACC_RUNNING);
}

static void test_gps_lost_aborts(void)
{
    acc_state_t st;
    acc_init(&st);
    for (uint32_t t = 0; t <= 1500; t += 100) acc_step(&st, t, 0, true, 10);
    uint32_t t = 1600;
    for (; t < 4000; t += 100) acc_step(&st, t, (t - 1550) / 1000.0f * 25, true, 10);  // 25 km/h per s
    CHECK(st.phase == ACC_RUNNING);
    CHECK(acc_step(&st, t + 900, 70, true, 10));  // 900 ms without samples
    CHECK(st.phase == ACC_DONE && st.res.aborted && st.res.t[ACC_T_0_100] < 0);
    acc_init(&st);
    for (uint32_t u = 0; u <= 1500; u += 100) acc_step(&st, u, 0, true, 10);
    acc_step(&st, 1600, 5, true, 10);
    CHECK(acc_step(&st, 1700, 0, false, 10));  // fix lost
    CHECK(st.res.aborted);
}

static void test_next_run_after_stopping(void)
{
    acc_state_t st;
    drive(6.0, 3000, 110, 100, 0, 10, &st);
    CHECK(st.phase == ACC_DONE);
    const float first = st.res.t[ACC_T_0_100];
    // slow down to 0, stand 1 s: armed again; the last result stays readable meanwhile
    uint32_t t = 20000;
    for (; t < 21500; t += 100) acc_step(&st, t, 0.3f, true, 10);
    CHECK(st.phase == ACC_ARMED && st.res.t[ACC_T_0_100] == first);
    acc_step(&st, t, 6, true, 10);
    CHECK(st.phase == ACC_RUNNING && st.res.t[ACC_T_0_100] < 0);
}

static void test_jump_start_clamped(void)
{
    // standing until 2.0 s, then 10 and 10.5 km/h: the line would point 20 s back; the start must
    // stay within one period of the last standing sample
    acc_state_t st;
    acc_init(&st);
    for (uint32_t t = 0; t <= 2000; t += 100) acc_step(&st, t, 0, true, 10);
    acc_step(&st, 2100, 10, true, 10);
    acc_step(&st, 2200, 10.5f, true, 10);
    CHECK(st.phase == ACC_RUNNING);
    CHECK(st.t0_ms >= 1900 && st.t0_ms <= 2100);
}

static void test_restart(void)
{
    acc_state_t st;
    // during a run: dropped, no result, back to waiting
    acc_init(&st);
    for (uint32_t t = 0; t <= 1500; t += 100) acc_step(&st, t, 0, true, 10);
    acc_step(&st, 1600, 5, true, 10);
    acc_step(&st, 1700, 10, true, 10);
    CHECK(st.phase == ACC_RUNNING);
    acc_restart(&st);
    CHECK(st.phase == ACC_IDLE && st.res.t[ACC_T_0_100] < 0 && acc_elapsed(&st, 5000) == 0);
    CHECK(!acc_step(&st, 1800, 15, true, 10));  // still moving: no run, no "finished"
    CHECK(st.phase == ACC_IDLE);
    // after a finished run: the result is cleared; standing 1 s arms again, not sooner
    drive(6.0, 3000, 110, 100, 0, 10, &st);
    CHECK(st.phase == ACC_DONE && st.res.t[ACC_T_0_100] > 0);
    uint32_t t = 30000;
    for (; t < 30500; t += 100) acc_step(&st, t, 0, true, 10);  // already standing 0.5 s
    acc_restart(&st);
    CHECK(st.phase == ACC_IDLE && st.res.t[ACC_T_0_100] < 0);
    for (; t < 31000; t += 100) acc_step(&st, t, 0, true, 10);
    CHECK(st.phase == ACC_IDLE);  // 0.5 s after the restart: not yet
    for (; t <= 31600; t += 100) acc_step(&st, t, 0, true, 10);
    CHECK(st.phase == ACC_ARMED);
    acc_step(&st, t, 5, true, 10);
    CHECK(st.phase == ACC_RUNNING);  // a new run starts normally
}

// Like drive(), with an altitude on every sample: the road rises grade_pct % along the distance,
// plus GPS-like noise: a slow wander (+-amp m over ~8 s) and +-1 m jitter per sample.
static acc_result_t drive_alt(double a, float v_end_kmh, double grade_pct, double amp, bool alt_ok,
                              acc_state_t *st)
{
    const uint32_t t_go = 3000;
    acc_init(st);
    bool done = false;
    unsigned seed = 12345;
    for (uint32_t t = 0; t < 120000 && !done; t += 100) {
        const double ts = t > t_go ? (t - t_go) / 1000.0 : 0;
        double v = a * ts * 3.6, d = 0.5 * a * ts * ts;
        if (v > v_end_kmh) {  // lift off: speed falls fast, distance hardly matters any more
            v = v_end_kmh - (v - v_end_kmh) * 3;
        }
        seed = seed * 1103515245u + 12345u;
        const double jitter = ((seed >> 16) % 2001) / 1000.0 - 1.0;  // -1..1 m
        const double h = 300 + grade_pct / 100 * d + amp * sin(t / 1300.0) + jitter;
        acc_set_alt(st, (float)h, alt_ok);
        done = acc_step(st, t, (float)v, true, 10);
    }
    return st->res;
}

static void test_grade(void)
{
    acc_state_t st;
    // 1/4 mile run, clean altitude: exact
    acc_result_t r = drive_alt(5.0, 250, 4.0, 0, true, &st);
    CHECK(r.grade_valid && NEAR(r.grade_pct, 4.0, 0.2));
    CHECK(NEAR(r.climb_m, 0.04 * r.dist_m, 1.0));
    r = drive_alt(5.0, 250, -3.0, 0, true, &st);
    CHECK(r.grade_valid && NEAR(r.grade_pct, -3.0, 0.2) && r.climb_m < 0);
    // noisy GPS altitude (+-3 m wander, +-1 m jitter): still within +-1.5 % over 400 m
    r = drive_alt(5.0, 250, 4.0, 3.0, true, &st);
    CHECK(r.grade_valid && NEAR(r.grade_pct, 4.0, 1.5));
    r = drive_alt(5.0, 250, 0.0, 3.0, true, &st);
    CHECK(r.grade_valid && NEAR(r.grade_pct, 0.0, 1.5));
    // no altitude: timed, but no grade
    r = drive_alt(5.0, 250, 4.0, 0, false, &st);
    CHECK(!r.grade_valid && r.t[ACC_T_0_100] > 0);
    // too short (lift at 30 km/h: ~7 m): no grade, the noise would be the answer
    r = drive_alt(5.0, 30, 4.0, 0, true, &st);
    CHECK(st.phase == ACC_DONE && !r.grade_valid);
}

static void test_grade_ignores_a_bad_start_altitude(void)
{
    // no altitude while standing, and the first moving reading is 8 m off (GPS glitch): the line is
    // fitted through all the run samples, so one bad reading at the start must not tilt it
    acc_state_t st;
    acc_init(&st);
    const double a = 5.0;
    for (uint32_t t = 0; t < 120000 && st.phase != ACC_DONE; t += 100) {
        const double ts = t > 3000 ? (t - 3000) / 1000.0 : 0;
        double v = a * ts * 3.6;
        if (v > 250) {
            v = 250 - (v - 250) * 3;
        }
        const double d = 0.5 * a * ts * ts;
        const bool first_moving = v >= ACC_TRIGGER_KMH && st.phase != ACC_RUNNING;
        acc_set_alt(&st, (float)(300 + 0.04 * d + (first_moving ? 8 : 0)), v >= ACC_TRIGGER_KMH);
        acc_step(&st, t, (float)v, true, 10);
    }
    CHECK(st.res.grade_valid && NEAR(st.res.grade_pct, 4.0, 0.3));
}

static void test_altitude_lost_mid_run(void)
{
    acc_state_t st;
    acc_init(&st);
    for (uint32_t t = 0; t <= 1500; t += 100) {
        acc_set_alt(&st, 300, true);
        acc_step(&st, t, 0, true, 10);
    }
    uint32_t t = 1600;
    for (; t < 20000 && st.phase != ACC_DONE; t += 100) {
        acc_set_alt(&st, 300, t != 5000);  // one sample without altitude
        acc_step(&st, t, t < 15000 ? (t - 1500) / 1000.0f * 15 : 1, true, 10);
    }
    CHECK(st.phase == ACC_DONE && st.res.dist_m > 100 && !st.res.grade_valid);
}

static void test_hist_line(void)
{
    acc_result_t r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < ACC_N; ++i) {
        r.t[i] = -1;
    }
    r.t[ACC_T_0_50] = 2.345f;
    r.t[ACC_T_0_100] = 6.5f;
    r.t[ACC_T_100M] = 7.91f;
    r.v_100m = 88.04f;
    r.v_quarter = 140;  // not reached: must stay empty
    r.v_max = 120.26f;
    r.dist_m = 180.4f;
    r.duration_s = 9.87f;
    char b[256];
    size_t l = acc_hist_line(b, sizeof b, "20261124-153012", "2026-11-24 15:30:12", &r, 10.04f);
    CHECK(l == strlen(b));
    CHECK(strcmp(b, "20261124-153012,2026-11-24 15:30:12,2.35,6.50,,,7.91,,88.0,,120.3,180.4,9.87,,,0,0,10.0\n") == 0);
    // same number of columns as the header
    int cols = 1, hcols = 1;
    for (const char *p = b; *p; ++p) cols += *p == ',';
    for (const char *p = acc_hist_header(); *p; ++p) hcols += *p == ',';
    CHECK(cols == hcols);
    r.grade_valid = true;
    r.grade_pct = -2.34f;
    r.climb_m = -4.2f;
    r.aborted = true;
    l = acc_hist_line(b, sizeof b, "R000003", "", &r, 1);
    CHECK(l > 0 && strstr(b, ",-2.3,-4.2,1,0,1.0\n") && strncmp(b, "R000003,,", 9) == 0);
    CHECK(acc_hist_line(b, 20, "R000003", "", &r, 1) == 0 && b[0] == 0);  // too small: nothing
}

static void test_hist_ids_and_stats(void)
{
    CHECK(acc_hist_valid_id("20261124-153012"));
    CHECK(acc_hist_valid_id("R000012"));
    CHECK(!acc_hist_valid_id("../runs.csv"));
    CHECK(!acc_hist_valid_id("20261124-15301"));
    CHECK(!acc_hist_valid_id("R00001x"));
    CHECK(!acc_hist_valid_id(""));
    CHECK(!acc_hist_valid_id(NULL));

    acc_stats_t s;
    memset(&s, 0, sizeof s);
    CHECK(acc_stats_mean(&s, ACC_T_0_100) < 0 && acc_stats_sd(&s, ACC_T_0_100) < 0);
    acc_result_t r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < ACC_N; ++i) r.t[i] = -1;
    const float t[3] = {6.0f, 6.5f, 7.0f};
    for (int k = 0; k < 3; ++k) {
        r.t[ACC_T_0_100] = t[k];
        acc_stats_add(&s, &r);
    }
    r.t[ACC_T_0_100] = 3.0f;  // interrupted run: counted as a run, not in the statistics
    r.aborted = true;
    acc_stats_add(&s, &r);
    r.aborted = false;
    r.low_rate = true;  // 1 Hz: same
    acc_stats_add(&s, &r);
    CHECK(s.runs == 5 && s.n[ACC_T_0_100] == 3 && s.n[ACC_T_0_50] == 0);
    CHECK(NEAR(acc_stats_mean(&s, ACC_T_0_100), 6.5, 1e-4));
    CHECK(NEAR(acc_stats_sd(&s, ACC_T_0_100), 0.5, 1e-3));
}

static void test_elapsed(void)
{
    acc_state_t st;
    acc_init(&st);
    for (uint32_t t = 0; t <= 1500; t += 100) acc_step(&st, t, 0, true, 10);
    acc_step(&st, 1600, 3.6f, true, 10);  // 1 m/s^2 from 1500 ms: started at 1500
    CHECK(NEAR(acc_elapsed(&st, 2500), 1.0, 0.01));
}

int main(void)
{
    test_constant_acceleration_10hz();
    test_start_not_on_a_sample();
    test_1hz_is_flagged_and_still_close();
    test_lift_off_ends_the_run();
    test_must_stand_before_a_run();
    test_gps_lost_aborts();
    test_next_run_after_stopping();
    test_jump_start_clamped();
    test_restart();
    test_grade();
    test_grade_ignores_a_bad_start_altitude();
    test_altitude_lost_mid_run();
    test_hist_line();
    test_hist_ids_and_stats();
    test_elapsed();
    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
