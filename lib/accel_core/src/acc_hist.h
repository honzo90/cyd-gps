// History of acceleration runs: CSV format of the run list on the SD card (/runs/runs.csv) and
// running statistics per metric (count, mean, spread) kept on the device. No Arduino: host-tested
// (test/host/test_accel.c). The phone reads the same CSV (src/web/runs.js).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "accel_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Header line of runs.csv (with '\n'). Columns: id, utc, the ACC_N times (s, empty = not reached),
// v100m, vquarter, vmax (km/h), dist_m, dur_s, grade_pct, climb_m (empty = unknown), aborted,
// low_rate (0/1), rate_hz.
const char *acc_hist_header(void);

// One run as a CSV line (with '\n'). id: "20261124-153012" (UTC of the end) or "R000012" without a
// GPS date; utc: "2026-11-24 15:30:12" or "". Returns the length, or 0 if buf is too small.
size_t acc_hist_line(char *buf, size_t n, const char *id, const char *utc, const acc_result_t *r, float rate_hz);

// Run ids that may name a file: "YYYYMMDD-HHMMSS" or "R" + 6 digits.
bool acc_hist_valid_id(const char *id);

// Runs that count for statistics and records: not interrupted, sampled at >= 5 Hz.
bool acc_hist_counts(const acc_result_t *r);

typedef struct {
    uint32_t runs;         // every saved run, also interrupted / 1 Hz ones
    uint16_t n[ACC_N];     // per metric: counted runs that reached it
    float sum[ACC_N], sq[ACC_N];
} acc_stats_t;

void acc_stats_add(acc_stats_t *s, const acc_result_t *r);
float acc_stats_mean(const acc_stats_t *s, int i);  // < 0 when no run
float acc_stats_sd(const acc_stats_t *s, int i);    // sample standard deviation; < 0 below 2 runs

#ifdef __cplusplus
}
#endif
