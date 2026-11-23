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
};

void web_begin();                              // starts the server task (once)
void web_publish(const web_snapshot_t *s);     // from the loop task, ~2x per second

// QR login. web_login_url() makes a new one-time code (valid 2 min, replaces the previous one) and
// writes "http://<ip>/login?t=<code>"; false when there is no network. Opening the link logs that
// browser in for 30 days (cookie), without the password. Call from the loop task.
static const uint32_t WEB_LOGIN_CODE_MS = 120000;
bool web_login_url(char *buf, size_t n);
void web_login_cancel();
void web_logout_all();  // forget every logged-in browser (the password keeps working)
