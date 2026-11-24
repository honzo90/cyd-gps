// Acceleration timer (0-100, 0-200, 100-200 km/h, 100 m, 1/4 mile) from GPS Doppler speed samples.
// No Arduino: host-tested (test/host/test_accel.c).
//
// Use: stand still -> ACC_ARMED; drive off -> ACC_RUNNING (the clock starts where the speed left 0,
// interpolated between the samples around the start); the run ends when the speed drops (lift off,
// braking), when every target was reached, or when samples stop coming. Crossing times are
// interpolated between samples, distance is integrated from speed (trapezoid).
//
// Road grade: GPS altitude is noisy (a few metres per reading), so the grade is the slope of a
// straight line fitted to altitude vs distance over every sample of the run, anchored by the
// altitude averaged while standing at the start. Reported from ACC_GRADE_MIN_M of run only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_STAND_KMH 2.0f      // below this the car stands (Doppler noise at rest is < 1 km/h)
#define ACC_STAND_MS 1000       // ... for this long before a run can start
#define ACC_TRIGGER_KMH 3.0f    // a sample at or above this starts the run
#define ACC_LIFT_KMH 5.0f       // the run ends once the speed is this far below its maximum
#define ACC_MAX_GAP_MS 500      // samples further apart (or 2.5 periods at low rates): aborted, GPS lost
#define ACC_MAX_RUN_MS 60000
#define ACC_DIST_SHORT_M 100.0f
#define ACC_DIST_QUARTER_M 402.336f  // 1/4 mile
#define ACC_GRADE_MIN_M 50.0f   // shorter runs: no grade (altitude noise would dominate)
#define ACC_GRADE_MIN_N 10      // ... and at least this many samples with altitude

typedef enum { ACC_IDLE = 0, ACC_ARMED, ACC_RUNNING, ACC_DONE } acc_phase_t;

enum { ACC_T_0_50 = 0, ACC_T_0_100, ACC_T_0_200, ACC_T_100_200, ACC_T_100M, ACC_T_QUARTER, ACC_N };

typedef struct {
    float t[ACC_N];      // seconds; < 0 = not reached
    float v_100m;        // km/h at 100 m and at 1/4 mile (0 if not reached)
    float v_quarter;
    float v_max;
    float dist_m;
    float duration_s;
    bool aborted;        // samples stopped (GPS lost): results up to that point
    bool low_rate;       // sampled at < 5 Hz: about +-0.1-0.5 s
    bool grade_valid;    // grade_pct / climb_m known (altitude on every sample, >= ACC_GRADE_MIN_M)
    float grade_pct;     // road grade over the run, % (+ uphill, - downhill)
    float climb_m;       // altitude change over the run by that grade (m)
} acc_result_t;

typedef struct {
    acc_phase_t phase;
    uint32_t stand_since;  // ms when the speed went below ACC_STAND_KMH (ARMED after ACC_STAND_MS)
    bool standing;
    bool have_prev;
    uint32_t prev_ms;
    float prev_kmh;
    double t0_ms;          // start of the clock (interpolated, so not a whole ms)
    uint32_t last_stand_ms;  // last sample below ACC_STAND_KMH
    bool t0_pending;       // only one moving sample so far: t0 is refined with the next one
    uint32_t p1_ms;        // that first moving sample
    float p1_kmh;
    float dist_m;
    // altitude: set by acc_set_alt() before each acc_step()
    float alt_m;
    bool alt_ok;
    float alt_stand;       // smoothed altitude while standing (start anchor of the grade fit)
    bool alt_stand_ok;
    bool alt_missing;      // a run sample without altitude: no grade for this run
    double alt_ref;        // fit is done on (altitude - alt_ref): keeps the sums small
    double fn, fd, fh, fdd, fdh;  // weighted least-squares sums: n, d, h, d*d, d*h
    acc_result_t res;      // current run while RUNNING, last run when DONE
} acc_state_t;

void acc_init(acc_state_t *s);

// One sample: receiver speed (km/h), when it arrived (ms), whether the fix is good, the measured
// sample rate (Hz). Returns true when a run has just finished (s->res holds it).
bool acc_step(acc_state_t *s, uint32_t t_ms, float kmh, bool valid, float rate_hz);

// Altitude (m, GPS) of the sample passed to the next acc_step(); ok = false when unknown.
// Without it the run is still timed, only the grade is missing.
void acc_set_alt(acc_state_t *s, float alt_m, bool ok);

// Restart: drop the current run or the last result and wait for a new start (stand 1 s again).
void acc_restart(acc_state_t *s);

// Seconds since the start while RUNNING (for the live clock), else the last run's duration.
float acc_elapsed(const acc_state_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
