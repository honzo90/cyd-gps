#include "accel_core.h"
#include <math.h>
#include <string.h>

static const float k_speed_targets[2] = {100.0f, 200.0f};
static const float k_dist_targets[2] = {ACC_DIST_SHORT_M, ACC_DIST_QUARTER_M};

void acc_init(acc_state_t *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < ACC_N; ++i) {
        s->res.t[i] = -1;
    }
}

// Where did the speed leave 0? The straight line through two moving samples (a then b), followed
// back to 0 km/h, clamped to [last standing sample - one period, a]. Samples below ACC_STAND_KMH are
// not used for the line: at rest they are Doppler noise, not the start of the motion.
static double start_time(const acc_state_t *s, uint32_t ta, float va, uint32_t tb, float vb)
{
    double t0 = ta;
    if (vb > va) {
        t0 = ta - va / ((vb - va) / (double)(tb - ta));
    }
    const double lo = (double)s->last_stand_ms - (double)(tb - ta);
    if (t0 < lo) {
        t0 = lo;
    }
    if (t0 > ta) {
        t0 = ta;
    }
    return t0;
}

static void start_run(acc_state_t *s, uint32_t t, float v)
{
    double t0;
    if (s->prev_kmh >= ACC_STAND_KMH) {  // two moving samples already (prev and this one)
        t0 = start_time(s, s->prev_ms, s->prev_kmh, t, v);
        s->t0_pending = false;
    } else {  // this is the first one: provisional until the next sample
        t0 = start_time(s, s->prev_ms, s->prev_kmh, t, v);
        s->t0_pending = true;
        s->p1_ms = t;
        s->p1_kmh = v;
    }
    memset(&s->res, 0, sizeof s->res);
    for (int i = 0; i < ACC_N; ++i) {
        s->res.t[i] = -1;
    }
    s->t0_ms = t0;
    s->dist_m = v / 2 / 3.6f * (float)((t - t0) / 1000.0);  // from 0 to v in a straight line
    s->res.v_max = v;
    s->phase = ACC_RUNNING;  // low_rate is set by run_step(), which always follows
}

static bool finish(acc_state_t *s, uint32_t last_ms, bool aborted)
{
    s->res.dist_m = s->dist_m;
    s->res.duration_s = (float)((last_ms - s->t0_ms) / 1000.0);
    s->res.aborted = aborted;
    s->phase = ACC_DONE;
    s->standing = false;
    return true;
}

// Fraction f of the step where the distance reaches `need` metres, speed linear vp -> v over dt_s.
static float dist_fraction(float vp, float v, float dt_s, float need)
{
    const float k = need * 3.6f / dt_s;  // = vp f + (v - vp) f^2 / 2
    const float dv = v - vp;
    if (fabsf(dv) < 1e-4f) {
        return vp > 0 ? k / vp : 1;
    }
    const float disc = vp * vp + 2 * dv * k;
    return disc < 0 ? 1 : (-vp + sqrtf(disc)) / dv;
}

static bool run_step(acc_state_t *s, uint32_t t, float v, float rate_hz)
{
    if (s->t0_pending) {
        s->t0_pending = false;
        s->t0_ms = start_time(s, s->p1_ms, s->p1_kmh, t, v);
        s->dist_m = s->p1_kmh / 2 / 3.6f * (float)((s->p1_ms - s->t0_ms) / 1000.0);
    }
    const float vp = s->prev_kmh;
    const float dt_s = (t - s->prev_ms) / 1000.0f;
    const float before = s->dist_m;
    s->dist_m += (vp + v) / 2 / 3.6f * dt_s;
    if (rate_hz < 5) {
        s->res.low_rate = true;
    }
    for (int i = 0; i < 2; ++i) {
        const float T = k_speed_targets[i];
        if (s->res.t[i] < 0 && vp < T && v >= T) {
            const double tc = s->prev_ms + (double)(T - vp) / (v - vp) * (t - s->prev_ms);
            s->res.t[i] = (float)((tc - s->t0_ms) / 1000.0);
        }
    }
    for (int i = 0; i < 2; ++i) {
        const int slot = i ? ACC_T_QUARTER : ACC_T_100M;
        const float D = k_dist_targets[i];
        if (s->res.t[slot] < 0 && before < D && s->dist_m >= D) {
            const float f = dist_fraction(vp, v, dt_s, D - before);
            s->res.t[slot] = (float)((s->prev_ms + (double)f * (t - s->prev_ms) - s->t0_ms) / 1000.0);
            const float vc = vp + (v - vp) * f;
            if (i) {
                s->res.v_quarter = vc;
            } else {
                s->res.v_100m = vc;
            }
        }
    }
    if (s->res.t[ACC_T_0_100] >= 0 && s->res.t[ACC_T_0_200] >= 0) {
        s->res.t[ACC_T_100_200] = s->res.t[ACC_T_0_200] - s->res.t[ACC_T_0_100];
    }
    if (v > s->res.v_max) {
        s->res.v_max = v;
    }
    const bool all = s->res.t[ACC_T_0_200] >= 0 && s->res.t[ACC_T_QUARTER] >= 0;
    if (all || v < ACC_STAND_KMH || v <= s->res.v_max - ACC_LIFT_KMH || t - s->t0_ms > ACC_MAX_RUN_MS) {
        return finish(s, t, false);
    }
    return false;
}

bool acc_step(acc_state_t *s, uint32_t t, float v, bool valid, float rate_hz)
{
    bool done = false;
    // a gap is > ACC_MAX_GAP_MS, or > 2.5 sample periods at a low rate (1 Hz: 2.5 s)
    const uint32_t max_gap = rate_hz > 0 && 2500 / rate_hz > ACC_MAX_GAP_MS ? (uint32_t)(2500 / rate_hz) : ACC_MAX_GAP_MS;
    const bool gap = s->have_prev && t - s->prev_ms > max_gap;
    if (!valid || gap) {
        if (s->phase == ACC_RUNNING) {
            done = finish(s, s->prev_ms, true);
        } else if (s->phase == ACC_ARMED) {
            s->phase = ACC_IDLE;
        }
        s->standing = false;
        s->have_prev = valid;  // a gap with a valid sample starts over from this sample
        s->prev_ms = t;
        s->prev_kmh = v;
        return done;
    }
    if (s->phase == ACC_RUNNING) {
        done = run_step(s, t, v, rate_hz);
    } else if (v < ACC_STAND_KMH) {
        s->last_stand_ms = t;
        if (!s->standing) {
            s->standing = true;
            s->stand_since = t;
        }
        if (t - s->stand_since >= ACC_STAND_MS) {
            s->phase = ACC_ARMED;
        }
    } else {
        s->standing = false;
        if (s->phase == ACC_ARMED && v >= ACC_TRIGGER_KMH && s->have_prev) {
            start_run(s, t, v);
        } else if (s->phase == ACC_ARMED && v >= ACC_TRIGGER_KMH) {
            s->phase = ACC_IDLE;
        }
        // ACC_ARMED below the trigger (creeping): stays armed; ACC_DONE keeps showing the last run
    }
    s->have_prev = true;
    s->prev_ms = t;
    s->prev_kmh = v;
    return done;
}

void acc_restart(acc_state_t *s)
{
    // IDLE, no result, not standing: the 1 s standing time starts again (arming refills the
    // previous sample, so nothing else needs to be kept)
    acc_init(s);
}

float acc_elapsed(const acc_state_t *s, uint32_t now_ms)
{
    if (s->phase == ACC_RUNNING) {
        return (float)((now_ms - s->t0_ms) / 1000.0);
    }
    return s->res.duration_s;
}
