// Web page on port 80 (same page on the hotspot and the home network), basic auth user "cyd" +
// device password. Live data, GPX files (download/delete), home Wi-Fi settings, firmware update.
// Runs in its own task; it never touches GPS/odometer state directly, only the snapshot below.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct web_snapshot_t {
    bool fix;
    double lat, lon;
    float speed_kmh, alt_m, hdop, max_kmh;
    uint8_t sats, sat_tracked, snr_top4;
    double trip_km, total_km;
    uint32_t moving_s;
    char utc[24];
    char gpx_file[32];  // file being written ("" = none): cannot be deleted
    uint32_t gpx_points;
    char sd[64];
    // acceleration timer: last run and best times (s, < 0 = none), see src/accel.h
    // order: 0-50, 0-100, 0-200, 100-200 km/h, 100 m, 1/4 mile (ACC_N in lib/accel_core)
    float acc_last[6], acc_best[6], acc_best_grade[6];  // grade % of the record run, NAN = unknown
    float acc_v100m, acc_vquarter, acc_vmax;
    float acc_grade, acc_climb;  // last run: road grade %, altitude change m; NAN = unknown
    bool acc_has_last, acc_running, acc_aborted, acc_low_rate;
    uint8_t rot;  // screen rotation 0..3 (x 90 degrees)
    bool dev;     // developer mode: technical details on the CYD screens
};

void web_begin();                              // starts the server task (once)
void web_publish(const web_snapshot_t *s);     // from the loop task, ~2x per second

// QR login. web_login_url() makes a new one-time code (valid 2 min, replaces the previous one) and
// writes "http://<ip>/login?t=<code>"; false when there is no network. Opening the link logs that
// browser in for 30 days (cookie), without the password. Call from the loop task.
static const uint32_t WEB_LOGIN_CODE_MS = 120000;
bool web_login_url(char *buf, size_t n);
void web_login_cancel();
void web_logout_all();

// Screen rotation chosen on the page (0..3), or -1. Taken (cleared) by the loop task.
int web_take_rotation();
// Developer mode switched on the page: 0/1 once, -1 = no request. The loop task applies it.
int web_take_dev();

// "Delete the run history" from the page; true once. The loop task does it (accel_clear_history).
bool web_take_clear_runs();  // forget every logged-in browser (the password keeps working)
