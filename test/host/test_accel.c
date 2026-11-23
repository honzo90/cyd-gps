// Host test for lib/accel_core. Run: sh test/host/run.sh
#include <math.h>
#include <stdio.h>
#include "accel_core.h"

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
    CHECK(NEAR(r.t[ACC_T_0_100], t_speed(a, 100), 0.03));
    CHECK(NEAR(r.t[ACC_T_0_200], t_speed(a, 200), 0.03));
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
    test_elapsed();
    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
