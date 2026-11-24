#include "accel.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include "gpx_log.h"
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

// Statistics of all runs (NVS "acc"/"stats"), and the speed trace of the run in progress. The
// trace stays in RAM during the run: writing the card then would delay loop() and with it the GPS
// timestamps the times are computed from. 700 x 4 B covers a 60 s run at 10 Hz.
static acc_stats_t s_stats;
struct trace_pt_t {
    uint16_t dt_ms;   // since s_tr_base
    uint16_t v_x100;  // km/h * 100
};
static trace_pt_t s_tr[700];
static uint16_t s_tr_n;
static uint32_t s_tr_base;

static void trace_add(uint32_t t_ms, float kmh)
{
    if (s_tr_n == 0) {
        s_tr_base = t_ms;
    }
    const uint32_t dt = t_ms - s_tr_base;
    if (s_tr_n < sizeof s_tr / sizeof s_tr[0] && dt <= UINT16_MAX) {
        s_tr[s_tr_n].dt_ms = (uint16_t)dt;
        s_tr[s_tr_n].v_x100 = (uint16_t)(kmh < 0 ? 0 : kmh > 650 ? 65000 : kmh * 100 + 0.5f);
        ++s_tr_n;
    }
}

static void save_stats()
{
#ifdef GPS_SIM
    return;
#endif
    Preferences p;
    if (p.begin("acc", false)) {
        p.putBytes("stats", &s_stats, sizeof s_stats);
        p.end();
    }
}

// /runs/runs.csv + /runs/<id>.csv. Called from the loop task right after the run (~50 ms of card
// writes; the GPS UART buffer holds ~350 ms).
static void save_run(const gps_data_t *g)
{
#ifdef GPS_SIM
    return;  // simulated runs are never history
#endif
    if (!gpx_log_sd_ok()) {
        Serial.println("ACC: no SD card, run not saved");
        return;
    }
    char id[20], utc[24] = "";
    if (g->date_valid && g->time_valid) {
        snprintf(id, sizeof id, "%04u%02u%02u-%02u%02u%02u", g->year, g->month, g->day, g->hh, g->mm, g->ss);
        snprintf(utc, sizeof utc, "%04u-%02u-%02u %02u:%02u:%02u", g->year, g->month, g->day, g->hh, g->mm, g->ss);
    } else {
        snprintf(id, sizeof id, "R%06lu", (unsigned long)(s_stats.runs % 1000000));
    }
    if (!SD.exists("/runs")) {
        SD.mkdir("/runs");
    }
    const bool fresh = !SD.exists("/runs/runs.csv");
    File f = SD.open("/runs/runs.csv", FILE_APPEND);
    char line[256];
    if (!f || !acc_hist_line(line, sizeof line, id, utc, &s_acc.res, s_rate)) {
        Serial.println("ACC: runs.csv not written");
        return;
    }
    if (fresh) {
        f.print(acc_hist_header());
    }
    f.print(line);
    f.close();
    // trace: from the start of the clock (0 km/h) to the end of the run
    char path[40];
    snprintf(path, sizeof path, "/runs/%s.csv", id);
    File t = SD.open(path, FILE_WRITE);
    if (!t) {
        Serial.println("ACC: trace not written");
        return;
    }
    t.print("t_s,kmh\n0.00,0.0\n");
    for (uint16_t i = 0; i < s_tr_n; ++i) {
        const double ts = ((double)s_tr_base + s_tr[i].dt_ms - s_acc.t0_ms) / 1000.0;
        if (ts > 0) {
            char b[24];
            snprintf(b, sizeof b, "%.2f,%.1f\n", ts, s_tr[i].v_x100 / 100.0);
            t.print(b);
        }
    }
    t.close();
    Serial.printf("ACC: saved run %s (%u samples)\n", id, s_tr_n);
}

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
    if (!ok || p.getBytes("stats", &s_stats, sizeof s_stats) != sizeof s_stats) {
        memset(&s_stats, 0, sizeof s_stats);
    }
    if (ok) {
        p.end();
    }
}

static void finished(const gps_data_t *g)
{
    const acc_result_t &r = s_acc.res;
    s_has_last = true;
    acc_stats_add(&s_stats, &r);
    save_stats();
    save_run(g);
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
    const acc_phase_t before = s_acc.phase;
    const bool done = acc_step(&s_acc, g->spd_ms, g->spd_kmh, s_ok, g->rate_hz);
    if (s_acc.phase == ACC_RUNNING && before != ACC_RUNNING) {
        s_tr_n = 0;  // a new run: new trace
    }
    if (s_acc.phase == ACC_RUNNING || (done && s_ok)) {
        trace_add(g->spd_ms, g->spd_kmh);  // also the sample that ended it
    }
    if (done) {
        finished(g);
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
    v->stats = s_stats;
}

void accel_clear_history()
{
    memset(&s_stats, 0, sizeof s_stats);
    save_stats();
    int n = 0;
    for (int batch = 0; batch < 100 && gpx_log_sd_ok(); ++batch) {
        File d = SD.open("/runs");
        int k = 0;
        if (d && d.isDirectory()) {
            // collect first: removing while iterating the directory confuses FATFS
            static char names[64][24];
            for (File f = d.openNextFile(); f && k < 64; f = d.openNextFile()) {
                const char *nm = strrchr(f.name(), '/');
                nm = nm ? nm + 1 : f.name();
                char id[24];
                snprintf(id, sizeof id, "%s", nm);
                char *dot = strrchr(id, '.');
                if (dot && strcmp(dot, ".csv") == 0) {
                    *dot = 0;
                    if (acc_hist_valid_id(id) || strcmp(id, "runs") == 0) {
                        snprintf(names[k++], sizeof names[0], "%s", nm);
                    }
                }
                f.close();
            }
            d.close();
            for (int i = 0; i < k; ++i) {
                char path[40];
                snprintf(path, sizeof path, "/runs/%s", names[i]);
                n += SD.remove(path) ? 1 : 0;
            }
        }
        if (k < 64) {
            break;  // that was the last batch
        }
    }
    Serial.printf("ACC: history cleared (%d files)\n", n);
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
