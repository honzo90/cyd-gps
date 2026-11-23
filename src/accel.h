// Acceleration timer on the CYD: feeds lib/accel_core with one sample per GPS epoch (10 Hz) and
// keeps the best times in NVS ("acc"), written only when a record improves.
#pragma once
#include "accel_core.h"
#include "gps.h"

struct accel_view_t {
    acc_phase_t phase;
    float elapsed_s;     // live clock while running
    acc_result_t last;   // current run while running, else the last finished one
    bool has_last;
    float best[ACC_N];   // < 0 = none yet
    float best_v100m, best_vquarter;  // speed at the end of the best 100 m / 1/4 mile
    float rate_hz;
    bool sample_ok;      // fix good enough to time
};

void accel_begin();
void accel_feed(const gps_data_t *g);  // from loop() for every new GPS epoch (spd_seq)
void accel_get(accel_view_t *v);
void accel_reset_best();
void accel_restart();  // drop the current run / last result, wait for a new start
