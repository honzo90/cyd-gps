#include "accel.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>

static acc_state_t s_acc;
static bool s_has_last;
static float s_best[ACC_N], s_best_v100m, s_best_vquarter;
static float s_rate;
static bool s_ok;

// NVS keys by name: adding a metric (0-50 in 1.0.16) keeps the stored bests of the others.
static const char *const k_keys[ACC_N] = {"t0_50", "t0_100", "t0_200", "t100_200", "t100m", "tquart"};
static const char *const k_gkeys[ACC_N] = {"g0_50", "g0_100", "g0_200", "g100_200", "g100m", "gquart"};
static float s_best_grade[ACC_N];

static bool good(const gps_data_t *g)
{
    // same quality gate as the odometer: a weak fix makes the Doppler speed jump
    return g->spd_valid && g->fix && g->sats >= 4 && g->hdop > 0 && g->hdop <= 4.0;
}

static void save_best()
{
#ifdef GPS_SIM
    return;  // simulated runs never become records
#endif
    Preferences p;
    if (p.begin("acc", false)) {
        for (int i = 0; i < ACC_N; ++i) {
            p.putFloat(k_keys[i], s_best[i]);
            p.putFloat(k_gkeys[i], s_best_grade[i]);
        }
        p.putFloat("v100m", s_best_v100m);
        p.putFloat("vquart", s_best_vquarter);
        p.end();
    }
}

void accel_begin()
{
    acc_init(&s_acc);
    Preferences p;
    const bool ok = p.begin("acc", false);  // read-write: creates the namespace on first boot (no write)
    for (int i = 0; i < ACC_N; ++i) {
        s_best[i] = ok ? p.getFloat(k_keys[i], -1) : -1;
        s_best_grade[i] = ok ? p.getFloat(k_gkeys[i], NAN) : NAN;
    }
    s_best_v100m = ok ? p.getFloat("v100m", 0) : 0;
    s_best_vquarter = ok ? p.getFloat("vquart", 0) : 0;
    if (ok) {
        p.end();
    }
}

static void finished()
{
    const acc_result_t &r = s_acc.res;
    s_has_last = true;
    char grade[32] = "grade ?";
    if (r.grade_valid) {
        snprintf(grade, sizeof grade, "grade %+.1f%% (%+.1f m)", r.grade_pct, r.climb_m);
    }
    Serial.printf("ACC: 0-50 %.2f  0-100 %.2f  0-200 %.2f  100-200 %.2f  100m %.2f@%.0f  1/4mi %.2f@%.0f  vmax %.0f  "
                  "%s  %s%s\n",
                  r.t[ACC_T_0_50], r.t[ACC_T_0_100], r.t[ACC_T_0_200], r.t[ACC_T_100_200], r.t[ACC_T_100M], r.v_100m,
                  r.t[ACC_T_QUARTER], r.v_quarter, r.v_max, grade, r.aborted ? "ABORTED " : "",
                  r.low_rate ? "LOW-RATE" : "");
    if (r.aborted || r.low_rate) {
        return;  // shown, but never a record
    }
    bool better = false;
    for (int i = 0; i < ACC_N; ++i) {
        if (r.t[i] > 0 && (s_best[i] < 0 || r.t[i] < s_best[i])) {
            s_best[i] = r.t[i];
            s_best_grade[i] = r.grade_valid ? r.grade_pct : NAN;  // a downhill record shows it
            better = true;
            if (i == ACC_T_100M) {
                s_best_v100m = r.v_100m;
            } else if (i == ACC_T_QUARTER) {
                s_best_vquarter = r.v_quarter;
            }
        }
    }
    if (better) {
        save_best();
    }
}

void accel_feed(const gps_data_t *g)
{
    s_rate = g->rate_hz;
    s_ok = good(g);
    acc_set_alt(&s_acc, (float)g->alt_m, s_ok && g->alt_valid);
    if (acc_step(&s_acc, g->spd_ms, g->spd_kmh, s_ok, g->rate_hz)) {
        finished();
    }
}

void accel_get(accel_view_t *v)
{
    v->phase = s_acc.phase;
    v->elapsed_s = acc_elapsed(&s_acc, millis());
    v->last = s_acc.res;
    v->has_last = s_has_last || s_acc.phase == ACC_RUNNING;
    memcpy(v->best, s_best, sizeof s_best);
    v->best_v100m = s_best_v100m;
    v->best_vquarter = s_best_vquarter;
    memcpy(v->best_grade, s_best_grade, sizeof s_best_grade);
    v->rate_hz = s_rate;
    v->sample_ok = s_ok;
}

void accel_restart()
{
    acc_restart(&s_acc);
    s_has_last = false;
    Serial.println("ACC: restart");
}

void accel_reset_best()
{
    for (float &b : s_best) {
        b = -1;
    }
    for (float &g : s_best_grade) {
        g = NAN;
    }
    s_best_v100m = s_best_vquarter = 0;
    save_best();
    Serial.println("ACC: best times cleared");
}
