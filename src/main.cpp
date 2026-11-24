// CYD2USB + NEO-8M GPS (Arduino-ESP32 2.0.x + LVGL 9). Four screens, swipe left/right:
//   1. Speedometer (start screen): big seven-segment km/h, bar, trip/max/satellites
//   2. Odometer: trip, total, max speed, moving time, average; long-press button resets the trip
//      and starts a new GPX file on the SD card
//   3. GPS: position, speed, altitude, satellites, time, signal
//   4. Wi-Fi: home network / hotspot, address of the web page, device password, on/off
// Touch calibration: runs on first boot, or press BOOT within 2 s AFTER power-up/reset.
// (Holding BOOT *during* reset puts the ESP32 into the serial bootloader - GPIO0 is a strapping pin.)
#include <Arduino.h>
#include <Preferences.h>
#include "cyd_arduino.h"
#include "cyd_calib_ui.h"
#include "gps.h"
#include "accel.h"
#include "bootlog.h"
#include "gh_ota.h"
#include "gpx_log.h"
#include "net.h"
#include "web.h"
#include "odometer.h"
#include "seg7.h"
#include "nav_core.h"
#include <math.h>

static const uint32_t k_bg = 0x101418, k_caption = 0x7F8A94, k_dim = 0x5A646E;

// Layout of the current rotation (see cyd_set_rotation): 320 x 240 or 240 x 320.
static int s_w = 320, s_h = 240;
static bool s_port;  // portrait: the status goes on its own line under the title
static int dy() { return s_port ? 26 : 0; }

// Speedometer
static seg7_t s_seg;
static lv_obj_t *s_spd_status, *s_spd_bar, *s_spd_scale, *s_spd_info, *s_dir_arrow, *s_dir_text;
static bool s_dir_shown;
static lv_point_precise_t s_arrow_pts[5];  // lv_line keeps a pointer: must stay alive

// Arrow 24 x 24 px pointing along the course (0 = up = north), clockwise like a compass.
static void set_arrow(float deg)
{
    static const float k_shape[5][2] = {{0, -10}, {7, 8}, {0, 3}, {-7, 8}, {0, -10}};
    const float r = deg * (float)M_PI / 180.0f, c = cosf(r), s = sinf(r);
    for (int i = 0; i < 5; ++i) {
        const float x = k_shape[i][0], y = k_shape[i][1];
        s_arrow_pts[i].x = (lv_value_precise_t)lroundf(12 + x * c - y * s);
        s_arrow_pts[i].y = (lv_value_precise_t)lroundf(12 + x * s + y * c);
    }
    lv_line_set_points(s_dir_arrow, s_arrow_pts, 5);
}
// Wi-Fi screen
static lv_obj_t *s_wifi_info, *s_wifi_btn_label, *s_gh_btn, *s_gh_btn_label, *s_chk_btn, *s_chk_btn_label;
// GPS screen
static lv_obj_t *s_status, *s_lat, *s_lon, *s_alt, *s_speed, *s_course, *s_sats, *s_hdop, *s_time, *s_diag;
// Odometer screen
static lv_obj_t *s_odo_status, *s_trip, *s_total, *s_max, *s_avg, *s_moving, *s_now, *s_reset_note, *s_sd;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Caption + value in a 2-column grid starting at y0.
static lv_obj_t *make_value(lv_obj_t *parent, const char *caption, int col, int row, int y0 = 36)
{
    const int x = 12 + col * (s_port ? 114 : 154), y = y0 + row * 44;
    make_label(parent, &lv_font_montserrat_14, k_caption, x, y, caption);
    return make_label(parent, &lv_font_montserrat_20, 0xFFFFFF, x, y + 16, "--");
}

// Fix status at the top right; in portrait on its own line (the title fills the narrow width).
static lv_obj_t *make_status(lv_obj_t *t)
{
    lv_obj_t *l = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 0, 0, "");
    if (s_port) {
        lv_obj_set_pos(l, 12, 34);
    } else {
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -12, 8);
    }
    return l;
}

static void set_status(const char *text, uint32_t color, bool r, bool g)
{
    for (lv_obj_t *o : {s_status, s_odo_status, s_spd_status}) {
        lv_label_set_text(o, text);
        lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    }
    cyd_led(r, g, false);
}

static void refresh_gps(const gps_data_t &g)
{
    // LVGL's own printf has no %f: format floats with the C library.
    char buf[32];
    if (g.fix) {
        snprintf(buf, sizeof buf, "%.6f %c", fabs(g.lat), g.lat >= 0 ? 'N' : 'S');
        lv_label_set_text(s_lat, buf);
        snprintf(buf, sizeof buf, "%.6f %c", fabs(g.lon), g.lon >= 0 ? 'E' : 'W');
        lv_label_set_text(s_lon, buf);
        snprintf(buf, sizeof buf, "%.1f m", g.alt_m);
        lv_label_set_text(s_alt, buf);
        snprintf(buf, sizeof buf, "%.1f km/h", g.speed_kmh);
        lv_label_set_text(s_speed, buf);
        snprintf(buf, sizeof buf, "%.0f\xC2\xB0", g.course_deg);  // degree sign, UTF-8
        lv_label_set_text(s_course, buf);
    } else {
        for (lv_obj_t *o : {s_lat, s_lon, s_alt, s_speed, s_course}) {
            lv_label_set_text(o, "--");
        }
    }
    lv_label_set_text_fmt(s_sats, "%u", (unsigned)g.sats);
    if (g.hdop > 0) {
        snprintf(buf, sizeof buf, "%.1f", g.hdop);
        lv_label_set_text(s_hdop, buf);
    } else {
        lv_label_set_text(s_hdop, "--");
    }
    if (g.time_valid) {
        lv_label_set_text_fmt(s_time, "%02u:%02u:%02u", g.hh, g.mm, g.ss);
    } else if (net_ntp_time(buf, sizeof buf)) {
        lv_label_set_text_fmt(s_time, "%s NTP", buf);
    } else {
        lv_label_set_text(s_time, "--");
    }
    if (g.baud == 0) {
        lv_label_set_text_fmt(s_diag, "SYMULACJA  |  NMEA %lu B", (unsigned long)g.chars);
    } else if (!g.baud_locked) {
        lv_label_set_text_fmt(s_diag, "%lu bd (szukam)  |  NMEA %lu B  |  bledne %lu", (unsigned long)g.baud,
                              (unsigned long)g.chars, (unsigned long)g.bad_sum);
    } else {
        // Signal: received/in view, count >= 30 dB-Hz, mean of the 4 strongest, max.
        lv_label_set_text_fmt(s_diag, "Sygnal: odb %u/%u  >=30dB: %u  top4 %u  %.0f Hz", g.sat_tracked,
                              g.sat_view, g.sat_strong, g.snr_top4, g.rate_hz);
    }
}

static void fmt_km(char *buf, size_t n, double m)
{
    const double km = m / 1000.0;
    snprintf(buf, n, km < 100 ? "%.2f km" : "%.1f km", km);
}

static void refresh_odo(const gps_data_t &g)
{
    const odo_state_t *o = odometer_get();
    char buf[32];
    fmt_km(buf, sizeof buf, o->trip_m);
    lv_label_set_text(s_trip, buf);
    snprintf(buf, sizeof buf, o->total_m < 100000 ? "%.2f km" : "%.0f km", o->total_m / 1000.0);
    lv_label_set_text(s_total, buf);
    snprintf(buf, sizeof buf, "%.1f km/h", o->max_kmh);
    lv_label_set_text(s_max, buf);
    if (o->moving_ms >= 1000) {
        snprintf(buf, sizeof buf, "%.1f km/h", o->trip_m / (o->moving_ms / 1000.0) * 3.6);
        lv_label_set_text(s_avg, buf);
    } else {
        lv_label_set_text(s_avg, "--");
    }
    const uint32_t s = o->moving_ms / 1000;
    lv_label_set_text_fmt(s_moving, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                          (unsigned long)(s % 60));
    if (g.fix) {
        snprintf(buf, sizeof buf, "%.1f km/h", g.speed_kmh);
        lv_label_set_text(s_now, buf);
    } else {
        lv_label_set_text(s_now, "--");
    }
    char sd[64];
    gpx_log_status(sd, sizeof sd);
    lv_label_set_text(s_sd, sd);
}

// Below this the receiver's speed is mostly noise: show 0 (same idea as the odometer's threshold).
static const float k_standing_kmh = 2.0f;

static void refresh_speedo(const gps_data_t &g)
{
    const odo_state_t *o = odometer_get();
    if (!g.fix) {
        seg7_set(&s_seg, -1, 0xFFC145);
        lv_bar_set_value(s_spd_bar, 0, LV_ANIM_OFF);
    } else {
        const float v = g.speed_kmh < k_standing_kmh ? 0.0f : (float)g.speed_kmh;
        const int shown = (int)lroundf(v);
        seg7_set(&s_seg, shown, 0xFFFFFF);
        // Bar scale steps up with speed so the bar stays readable on foot and in a car.
        const int scale = shown <= 30 ? 30 : shown <= 60 ? 60 : shown <= 120 ? 120 : 200;
        lv_bar_set_range(s_spd_bar, 0, scale);
        lv_bar_set_value(s_spd_bar, shown > scale ? scale : shown, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_spd_scale, "0 - %d", scale);
    }
    if (nav_show(&s_dir_shown, g.fix, g.course_valid, (float)g.speed_kmh)) {
        set_arrow((float)g.course_deg);
        lv_obj_remove_flag(s_dir_arrow, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_dir_text, "%s %d\xC2\xB0", nav_cardinal((float)g.course_deg), nav_deg((float)g.course_deg));
    } else {
        lv_obj_add_flag(s_dir_arrow, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_dir_text, "--");
    }
    char buf[64];
    snprintf(buf, sizeof buf, s_port ? "Trip %.2f km  Max %.0f  Sat %u" : "Trip %.2f km    Max %.0f    Sat %u",
             o->trip_m / 1000.0, o->max_kmh, (unsigned)g.sats);
    lv_label_set_text(s_spd_info, buf);
}

static void show(lv_obj_t *o, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_wifi()
{
    net_status_t n;
    net_get(&n);
    lv_label_set_text(s_wifi_btn_label, n.enabled ? "Wylacz Wi-Fi" : "Wlacz Wi-Fi");
    char info[400];
    int len = 0;
    if (!n.enabled) {
        len = snprintf(info, sizeof info, "Wi-Fi wylaczone.\n\nWlacz, aby pobierac trasy GPX,\nwidziec dane na telefonie\ni aktualizowac firmware.");
    } else {
        if (n.sta_connected) {
            len += snprintf(info + len, sizeof info - len, "Siec: %s  (%d dBm)\nStrona: http://%s\n", n.sta_ssid,
                            n.rssi, n.sta_ip);
        } else if (n.sta_configured) {
            len += snprintf(info + len, sizeof info - len, "Siec: %s - %s\n", n.sta_ssid,
                            n.sta_auth_failed ? "ZLE HASLO" : "laczenie...");
        } else {
            len += snprintf(info + len, sizeof info - len, "Siec domowa: nie ustawiona\n");
        }
        if (n.ap_on) {
            len += snprintf(info + len, sizeof info - len, "Hotspot: %s  (%u pol.)\nStrona: http://%s\n", n.ap_ssid,
                            n.ap_clients, n.ap_ip);
        } else {
            len += snprintf(info + len, sizeof info - len, "Hotspot: wylaczony\n");
        }
        len += snprintf(info + len, sizeof info - len, "Login: %s   Haslo: ******  (QR)\n", net_user());
        len += snprintf(info + len, sizeof info - len, "Czas NTP: %s   FW %s\n", n.ntp_ok ? "tak" : "nie", FW_VERSION);
        gh_ota_text(info + len, sizeof info - len);
    }
    lv_label_set_text(s_wifi_info, info);
    gh_status_t gh;
    gh_ota_get(&gh);
    // Same slot: "Instaluj x.y.z" when a newer release is known, otherwise "Sprawdz aktualizacje"
    // (only in the home network: the hotspot has no internet).
    const bool avail = gh.state == GH_AVAILABLE;
    const bool can_check = n.enabled && n.sta_connected && !avail && gh.state != GH_INSTALLING;
    if (avail) {
        lv_label_set_text_fmt(s_gh_btn_label, "Instaluj %s", gh.latest);
    }
    show(s_gh_btn, avail);
    show(s_chk_btn, can_check);
    const bool checking = gh.state == GH_CHECKING;
    lv_obj_set_state(s_chk_btn, LV_STATE_DISABLED, checking);
    lv_label_set_text(s_chk_btn_label, checking ? "Sprawdzam..." : "Sprawdz aktualizacje");
}

static void publish_web(const gps_data_t &g)
{
    const odo_state_t *o = odometer_get();
    web_snapshot_t w = {};
    w.fix = g.fix;
    w.lat = g.lat;
    w.lon = g.lon;
    w.speed_kmh = (float)g.speed_kmh;
    w.alt_m = (float)g.alt_m;
    w.hdop = (float)g.hdop;
    w.max_kmh = o->max_kmh;
    w.sats = (uint8_t)g.sats;
    w.sat_tracked = g.sat_tracked;
    w.snr_top4 = g.snr_top4;
    w.trip_km = o->trip_m / 1000.0;
    w.total_km = o->total_m / 1000.0;
    w.moving_s = o->moving_ms / 1000;
    char t[12];
    if (g.time_valid && g.date_valid) {
        snprintf(w.utc, sizeof w.utc, "%04u-%02u-%02u %02u:%02u:%02u", g.year, g.month, g.day, g.hh, g.mm, g.ss);
    } else if (net_ntp_time(t, sizeof t)) {
        snprintf(w.utc, sizeof w.utc, "%s (NTP)", t);
    }
    w.gpx_points = gpx_log_current(w.gpx_file, sizeof w.gpx_file);
    gpx_log_status(w.sd, sizeof w.sd);
    accel_view_t a;
    accel_get(&a);
    for (int i = 0; i < ACC_N; ++i) {
        w.acc_last[i] = a.has_last ? a.last.t[i] : -1;
        w.acc_best[i] = a.best[i];
        w.acc_best_grade[i] = a.best_grade[i];
    }
    static_assert(ACC_N == sizeof w.acc_last / sizeof w.acc_last[0], "web snapshot size");
    w.acc_grade = a.has_last && a.last.grade_valid ? a.last.grade_pct : NAN;
    w.acc_climb = a.has_last && a.last.grade_valid ? a.last.climb_m : NAN;
    w.acc_v100m = a.last.v_100m;
    w.acc_vquarter = a.last.v_quarter;
    w.acc_vmax = a.last.v_max;
    w.acc_has_last = a.has_last;
    w.acc_running = a.phase == ACC_RUNNING;
    w.acc_aborted = a.last.aborted;
    w.acc_low_rate = a.last.low_rate;
    w.rot = cyd_rotation();
    web_publish(&w);
}

static void refresh(lv_timer_t *)
{
    gps_data_t g;
    gps_get(&g);
    if (g.chars == 0) {
        set_status("BRAK DANYCH", 0xFF5A5A, true, false);
    } else if (!g.baud_locked) {
        set_status("SZUKAM BAUD", 0xFF5A5A, true, false);
    } else if (!g.fix) {
        set_status("SZUKAM FIX...", 0xFFC145, true, true);
    } else {
        set_status("FIX", 0x4CD97B, false, true);
    }
    refresh_gps(g);
    refresh_odo(g);
    refresh_speedo(g);
    refresh_wifi();
    publish_web(g);
}

static void hide_note(lv_timer_t *t)
{
    lv_obj_add_flag(s_reset_note, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
}

static void on_reset_trip(lv_event_t *)
{
    odometer_reset_trip();
    gpx_log_new_track();
    lv_obj_remove_flag(s_reset_note, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(hide_note, 2000, NULL);
    refresh(NULL);
}

// ---- POMIAR: acceleration timer ------------------------------------------------------------
static const char *const k_acc_names[ACC_N] = {"0-50 km/h",    "0-100 km/h", "0-200 km/h",
                                               "100-200 km/h", "100 m",      "1/4 mili"};

// "pod gore 2.1%", "z gorki 1.4%", "plasko" (under 0.5 %: within what GPS altitude can tell)
static void fmt_grade(char *buf, size_t n, const acc_result_t &r)
{
    if (!r.grade_valid) {
        buf[0] = 0;
    } else if (fabsf(r.grade_pct) < 0.5f) {
        snprintf(buf, n, "plasko");
    } else {
        snprintf(buf, n, "%s %.1f%%", r.grade_pct > 0 ? "pod gore" : "z gorki", fabsf(r.grade_pct));
    }
}
static lv_obj_t *s_acc_status, *s_acc_clock, *s_acc_speed, *s_acc_val[ACC_N], *s_acc_best[ACC_N], *s_acc_note;
static lv_timer_t *s_acc_timer;  // deleted with the UI on a rotation

static void fmt_time(char *buf, size_t n, float t)
{
    if (t < 0) {
        snprintf(buf, n, "--");
    } else {
        snprintf(buf, n, "%.2f s", t);
    }
}

static void refresh_accel(lv_timer_t *)
{
    accel_view_t a;
    accel_get(&a);
    gps_data_t g;
    gps_get(&g);
    const char *st;
    uint32_t col;
    if (!a.sample_ok) {
        st = "BRAK FIX";
        col = 0xFF5A5A;
    } else if (a.rate_hz > 0 && a.rate_hz < 5) {
        st = "GPS 1 Hz";
        col = 0xFFC145;
    } else if (a.phase == ACC_RUNNING) {
        st = "POMIAR";
        col = 0xFF7A1A;
    } else if (a.phase == ACC_ARMED) {
        st = "GOTOWY";
        col = 0x4CD97B;
    } else {
        st = "ZATRZYMAJ SIE";
        col = 0x9AA4AE;
    }
    lv_label_set_text(s_acc_status, st);
    lv_obj_set_style_text_color(s_acc_status, lv_color_hex(col), 0);

    char b[24];
    fmt_time(b, sizeof b, a.phase == ACC_RUNNING || a.has_last ? a.elapsed_s : 0);
    lv_label_set_text(s_acc_clock, b);
    lv_obj_set_style_text_color(s_acc_clock, lv_color_hex(a.phase == ACC_RUNNING ? 0xFF7A1A : 0xFFFFFF), 0);
    lv_label_set_text_fmt(s_acc_speed, "%d km/h  %.0f Hz", (int)(g.spd_kmh + 0.5f), a.rate_hz);

    for (int i = 0; i < ACC_N; ++i) {
        fmt_time(b, sizeof b, a.has_last ? a.last.t[i] : -1);
        lv_label_set_text(s_acc_val[i], b);
        if (a.best[i] > 0) {
            lv_label_set_text_fmt(s_acc_best[i], "naj. %.2f", a.best[i]);
        } else {
            lv_label_set_text(s_acc_best[i], "");
        }
    }
    const bool shown = a.has_last && a.phase != ACC_RUNNING;  // a finished run on screen
    if (shown && a.last.aborted) {
        lv_label_set_text(s_acc_note, "Przerwany: utrata GPS");
    } else if (shown && a.last.low_rate) {
        lv_label_set_text(s_acc_note, "GPS 1 Hz: wynik przyblizony");
    } else if (shown) {  // summary with the road grade (downhill runs are faster)
        char g[24], line[64];
        fmt_grade(g, sizeof g, a.last);
        if (s_port) {
            snprintf(line, sizeof line, "%.0f m%s%s", a.last.dist_m, g[0] ? ", " : "", g);
        } else {
            snprintf(line, sizeof line, "Vmax %.0f km/h, %.0f m%s%s", a.last.v_max, a.last.dist_m, g[0] ? ", " : "", g);
        }
        lv_label_set_text(s_acc_note, line);
    } else if (a.phase == ACC_ARMED && !a.stats.runs) {
        lv_label_set_text(s_acc_note, "Ruszaj - czas startuje sam");
    } else if (a.stats.runs) {  // short statistics; the full history is on the web page
        const float m = acc_stats_mean(&a.stats, ACC_T_0_100);
        if (m > 0) {
            char line[64];
            snprintf(line, sizeof line, "Pomiary: %lu, sr. 0-100 %.2f s", (unsigned long)a.stats.runs, m);
            lv_label_set_text(s_acc_note, line);
        } else {
            lv_label_set_text_fmt(s_acc_note, "Pomiary: %lu", (unsigned long)a.stats.runs);
        }
    } else {
        lv_label_set_text(s_acc_note, "Tylko na zamknietym torze.");
    }
}

static void on_acc_restart(lv_event_t *)
{
    accel_restart();
    refresh_accel(NULL);
}

static void on_acc_reset(lv_event_t *)
{
    accel_reset_best();
    refresh_accel(NULL);
}

static void build_accel_screen(lv_obj_t *t)
{
    lv_obj_t *title = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, LV_SYMBOL_LEFT "  POMIAR  " LV_SYMBOL_RIGHT);
    // long press on the title clears the best times (rare, so no extra button)
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, on_acc_reset, LV_EVENT_LONG_PRESSED, NULL);
    s_acc_status = make_status(t);

    s_acc_clock = make_label(t, &lv_font_montserrat_32, 0xFFFFFF, 12, 38 + dy(), "0.00 s");
    s_acc_speed = make_label(t, &lv_font_montserrat_14, k_caption, s_port ? 12 : 130, s_port ? 100 : 52, "");

    // Restart: drop the current run or the last result, then stand 1 s and go again
    lv_obj_t *rb = lv_button_create(t);
    lv_obj_set_size(rb, 84, 34);
    lv_obj_align(rb, LV_ALIGN_TOP_RIGHT, s_port ? -8 : -10, s_port ? 64 : 40);
    lv_obj_set_style_bg_color(rb, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_color(rb, lv_color_hex(0x8A2E2E), LV_STATE_PRESSED);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Restart");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rb, on_acc_restart, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < ACC_N; ++i) {
        if (s_port) {  // name with the best time under it, value on the right
            const int y = 120 + i * 30;
            make_label(t, &lv_font_montserrat_14, k_caption, 12, y, k_acc_names[i]);
            s_acc_best[i] = make_label(t, &lv_font_montserrat_14, k_dim, 12, y + 15, "");
            s_acc_val[i] = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 132, y + 4, "--");
        } else {
            const int y = 80 + i * 22;
            make_label(t, &lv_font_montserrat_14, k_caption, 12, y + 4, k_acc_names[i]);
            s_acc_val[i] = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 118, y, "--");
            s_acc_best[i] = make_label(t, &lv_font_montserrat_14, k_dim, 224, y + 4, "");
        }
    }
    s_acc_note = make_label(t, &lv_font_montserrat_14, 0x9AA4AE, 0, 0, "");
    lv_obj_set_width(s_acc_note, s_w - 16);
    lv_label_set_long_mode(s_acc_note, LV_LABEL_LONG_DOT);  // one line, also in portrait
    lv_obj_set_style_text_align(s_acc_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_acc_note, LV_ALIGN_BOTTOM_MID, 0, s_port ? -2 : -4);
    // 100 ms: the clock runs smoothly while the rest of the UI refreshes every 500 ms
    s_acc_timer = lv_timer_create(refresh_accel, 100, NULL);
}

static void build_speedo_screen(lv_obj_t *t)
{
    make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, "PREDKOSC  " LV_SYMBOL_RIGHT);
    s_spd_status = make_status(t);

    // Landscape: 3 digits of 76 x 128 px, 16 px stroke: 3*76 + 2*14 = 256 px wide, centred.
    // Portrait: 66 x 120 px, 14 px stroke: 3*66 + 2*10 = 218 px in the 240 px width.
    const int dw = s_port ? 218 : 256, mx = (s_w - dw) / 2;  // digits / bar width, left margin
    if (s_port) {
        seg7_create(&s_seg, t, mx, 68, 66, 120, 14, 10);
    } else {
        seg7_create(&s_seg, t, mx, 38, 76, 128, 16, 14);
    }
    const int y_scale = s_port ? 200 : 170;  // row under the digits: scale, (direction,) unit

    lv_obj_t *unit = make_label(t, &lv_font_montserrat_20, 0x7F8A94, 0, 0, "km/h");
    lv_obj_align(unit, LV_ALIGN_TOP_RIGHT, -mx, y_scale);
    s_spd_scale = make_label(t, &lv_font_montserrat_14, 0x5A646E, mx, y_scale + 4, "");

    s_spd_bar = lv_bar_create(t);
    lv_obj_set_size(s_spd_bar, dw, 10);
    lv_obj_set_pos(s_spd_bar, mx, y_scale + 26);
    lv_obj_set_style_bg_color(s_spd_bar, lv_color_hex(0x1A2026), 0);
    lv_obj_set_style_bg_color(s_spd_bar, lv_color_hex(0x4CD97B), LV_PART_INDICATOR);
    lv_obj_remove_flag(s_spd_bar, LV_OBJ_FLAG_CLICKABLE);

    // direction of travel (GPS course, shown from 5 km/h): arrow + "NE 47°" between scale and unit
    s_dir_arrow = lv_line_create(t);
    lv_obj_set_size(s_dir_arrow, 24, 24);
    // landscape: between scale and unit; portrait: own row under the bar
    lv_obj_set_pos(s_dir_arrow, s_port ? 72 : 112, s_port ? 252 : 167);
    lv_obj_set_style_line_width(s_dir_arrow, 3, 0);
    lv_obj_set_style_line_color(s_dir_arrow, lv_color_hex(0x4CD97B), 0);
    lv_obj_set_style_line_rounded(s_dir_arrow, true, 0);
    set_arrow(0);
    lv_obj_add_flag(s_dir_arrow, LV_OBJ_FLAG_HIDDEN);
    s_dir_text = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, s_port ? 102 : 142, s_port ? 253 : 168, "--");

    s_spd_info = make_label(t, &lv_font_montserrat_14, 0x9AA4AE, 0, 0, "");
    lv_obj_align(s_spd_info, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void on_wifi_toggle(lv_event_t *)
{
    net_status_t n;
    net_get(&n);
    net_set_enabled(!n.enabled);
    refresh_wifi();
}

static void on_gh_install(lv_event_t *)
{
    gh_ota_install();
}

// ---- QR login overlay: one-time link + password, closes itself after WEB_LOGIN_CODE_MS ----
static lv_obj_t *s_qr_panel, *s_qr_left;
static lv_timer_t *s_qr_timer;
static uint32_t s_qr_at;

static void qr_close()
{
    if (!s_qr_panel) {
        return;
    }
    web_login_cancel();
    lv_timer_delete(s_qr_timer);
    lv_obj_delete(s_qr_panel);  // frees the QR canvas
    s_qr_panel = NULL;
    s_qr_timer = NULL;
}

static void qr_tick(lv_timer_t *)
{
    const uint32_t gone = millis() - s_qr_at;
    if (gone >= WEB_LOGIN_CODE_MS) {
        qr_close();
        return;
    }
    const uint32_t left = (WEB_LOGIN_CODE_MS - gone + 999) / 1000;
    lv_label_set_text_fmt(s_qr_left, "Wazny %lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
}

static void on_qr_close(lv_event_t *)
{
    qr_close();
}

static void on_logout_all(lv_event_t *)
{
    web_logout_all();
    qr_close();
}

static void on_qr_open(lv_event_t *)
{
    char url[96];
    if (s_qr_panel || !web_login_url(url, sizeof url)) {
        return;
    }
    net_status_t n;
    net_get(&n);
    s_qr_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_qr_panel);
    lv_obj_set_size(s_qr_panel, s_w, s_h);
    lv_obj_set_style_bg_color(s_qr_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(s_qr_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_qr_panel, LV_OBJ_FLAG_CLICKABLE);  // swallow touches meant for the tiles below

    // White square with a quiet zone: phones read it much more reliably.
    lv_obj_t *bg = lv_obj_create(s_qr_panel);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, 176, 176);
    lv_obj_set_pos(bg, s_port ? (s_w - 176) / 2 : 8, 8);
    lv_obj_set_style_bg_color(bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_t *qr = lv_qrcode_create(bg);
    lv_qrcode_set_size(qr, 160);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_center(qr);

    if (s_port) {  // portrait: text under the code
        lv_obj_t *info = make_label(s_qr_panel, &lv_font_montserrat_14, 0xC8D0D8, 12, 192, "");
        lv_label_set_text_fmt(info, "Zeskanuj telefonem: strona bez\nhasla przez 30 dni.\nHaslo (%s): %s", net_user(),
                              n.dev_pass);
        s_qr_left = make_label(s_qr_panel, &lv_font_montserrat_14, 0xFFB347, 12, 250, "");
    } else {
        make_label(s_qr_panel, &lv_font_montserrat_20, 0xFFFFFF, 196, 10, "Zeskanuj\ntelefonem");
        lv_obj_t *info = make_label(s_qr_panel, &lv_font_montserrat_14, 0xC8D0D8, 196, 66, "");
        lv_label_set_text_fmt(info, "Strona bez hasla\nprzez 30 dni.\n\nHaslo (%s):\n%s", net_user(), n.dev_pass);
        s_qr_left = make_label(s_qr_panel, &lv_font_montserrat_14, 0xFFB347, 196, 160, "");
    }

    lv_obj_t *close = lv_button_create(s_qr_panel);
    lv_obj_set_size(close, 96, 34);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x2A3138), 0);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Zamknij");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, on_qr_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *out = lv_button_create(s_qr_panel);
    lv_obj_set_size(out, 104, 34);
    lv_obj_align(out, LV_ALIGN_BOTTOM_RIGHT, -112, -6);
    lv_obj_set_style_bg_color(out, lv_color_hex(0x5A2424), 0);
    lv_obj_t *ol = lv_label_create(out);
    lv_label_set_text(ol, "Wyloguj tel.");
    lv_obj_center(ol);
    lv_obj_add_event_cb(out, on_logout_all, LV_EVENT_LONG_PRESSED, NULL);  // all phones: long press

    s_qr_at = millis();
    s_qr_timer = lv_timer_create(qr_tick, 500, NULL);
    qr_tick(NULL);
}

// Rotation is applied from loop(), never inside an event: it deletes and rebuilds the whole UI,
// including the button that was just tapped. Set by the button and by the web page.
static volatile int s_rot_req = -1;

static void on_rotate(lv_event_t *)
{
    s_rot_req = (cyd_rotation() + 1) & 3;
}

static void on_gh_check(lv_event_t *)
{
    gh_ota_check_now();
    lv_obj_add_state(s_chk_btn, LV_STATE_DISABLED);  // feedback before the task picks it up
    lv_label_set_text(s_chk_btn_label, "Sprawdzam...");
}

static void build_wifi_screen(lv_obj_t *t)
{
    make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, LV_SYMBOL_LEFT "  WI-FI");
    s_wifi_info = make_label(t, &lv_font_montserrat_14, 0xE8ECEF, 12, 42, "");

    // QR login: long press, so the password and code are not shown by a stray touch.
    lv_obj_t *qr = lv_button_create(t);
    lv_obj_set_size(qr, 56, 30);
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(qr, lv_color_hex(0x2A3138), 0);
    lv_obj_t *ql = lv_label_create(qr);
    lv_label_set_text(ql, "QR");
    lv_obj_center(ql);
    lv_obj_add_event_cb(qr, on_qr_open, LV_EVENT_LONG_PRESSED, NULL);

    // Rotate the screen by 90 degrees per tap (remembered; also on the web page).
    lv_obj_t *rot = lv_button_create(t);
    lv_obj_set_size(rot, 56, 30);
    lv_obj_align(rot, LV_ALIGN_TOP_RIGHT, -70, 6);
    lv_obj_set_style_bg_color(rot, lv_color_hex(0x2A3138), 0);
    lv_obj_t *rl = lv_label_create(rot);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rot, on_rotate, LV_EVENT_CLICKED, NULL);

    lv_obj_set_width(s_wifi_info, s_w - 24);
    lv_obj_set_style_text_line_space(s_wifi_info, s_port ? 2 : 6, 0);
    if (s_port) {  // never under the two stacked buttons (they start at y 240)
        lv_obj_set_height(s_wifi_info, 240 - 4 - 42);
        lv_label_set_long_mode(s_wifi_info, LV_LABEL_LONG_CLIP);
    }
    // Buttons: landscape side by side; portrait full width, update above Wi-Fi on/off.
    const int bw_upd = s_port ? s_w - 16 : 160, bw_wifi = s_port ? s_w - 16 : 140;

    // Install from GitHub: long press, so a stray touch while driving cannot start it.
    s_gh_btn = lv_button_create(t);
    lv_obj_set_size(s_gh_btn, bw_upd, 34);
    lv_obj_align(s_gh_btn, LV_ALIGN_BOTTOM_LEFT, 8, s_port ? -46 : -6);
    lv_obj_set_style_bg_color(s_gh_btn, lv_color_hex(0x1F5A36), 0);
    s_gh_btn_label = lv_label_create(s_gh_btn);
    lv_obj_center(s_gh_btn_label);
    lv_obj_add_event_cb(s_gh_btn, on_gh_install, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_gh_btn, LV_OBJ_FLAG_HIDDEN);

    // Check for updates: a plain click (it only asks GitHub, installs nothing).
    s_chk_btn = lv_button_create(t);
    lv_obj_set_size(s_chk_btn, bw_upd, 34);
    lv_obj_align(s_chk_btn, LV_ALIGN_BOTTOM_LEFT, 8, s_port ? -46 : -6);
    lv_obj_set_style_bg_color(s_chk_btn, lv_color_hex(0x2A3138), 0);
    s_chk_btn_label = lv_label_create(s_chk_btn);
    lv_obj_center(s_chk_btn_label);
    lv_obj_add_event_cb(s_chk_btn, on_gh_check, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_chk_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn = lv_button_create(t);
    lv_obj_set_size(btn, bw_wifi, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3138), 0);
    s_wifi_btn_label = lv_label_create(btn);
    lv_obj_center(s_wifi_btn_label);
    lv_obj_add_event_cb(btn, on_wifi_toggle, LV_EVENT_CLICKED, NULL);
}

static void build_gps_screen(lv_obj_t *t)
{
    make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, LV_SYMBOL_LEFT "  GPS  " LV_SYMBOL_RIGHT);
    s_status = make_status(t);
    // portrait: latitude and longitude need a full row each, the rest is shifted down by one row
    const int y0 = 36 + dy(), r = s_port ? 1 : 0;
    s_lat = make_value(t, "Szerokosc", 0, 0, y0);
    s_lon = make_value(t, "Dlugosc", s_port ? 0 : 1, r, y0);
    s_alt = make_value(t, "Wysokosc", 0, 1 + r, y0);
    s_speed = make_value(t, "Predkosc", 1, 1 + r, y0);
    s_sats = make_value(t, "Satelity", 0, 2 + r, y0);
    s_hdop = make_value(t, "HDOP", 1, 2 + r, y0);
    s_course = make_value(t, "Kurs", 0, 3 + r, y0);
    s_time = make_value(t, "Czas UTC", 1, 3 + r, y0);
    s_diag = make_label(t, &lv_font_montserrat_14, k_dim, 0, 0, "");
    lv_obj_set_width(s_diag, s_w - 16);  // wraps to two lines in portrait
    lv_obj_set_style_text_align(s_diag, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_diag, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_odo_screen(lv_obj_t *t)
{
    make_label(t, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, LV_SYMBOL_LEFT "  LICZNIK  " LV_SYMBOL_RIGHT);
    s_odo_status = make_status(t);

    make_label(t, &lv_font_montserrat_14, k_caption, 12, 38 + dy(), "Trip");
    s_trip = make_label(t, &lv_font_montserrat_32, 0xFFFFFF, 12, 54 + dy(), "--");
    // total: right of the trip in landscape, under it in portrait
    make_label(t, &lv_font_montserrat_14, k_caption, s_port ? 12 : 166, s_port ? 118 : 38, "Calkowity");
    s_total = make_label(t, &lv_font_montserrat_20, 0xFFFFFF, s_port ? 12 : 166, s_port ? 134 : 58, "--");

    const int y0 = s_port ? 166 : 98;
    s_max = make_value(t, "Maks. predkosc", 0, 0, y0);
    s_avg = make_value(t, s_port ? "Srednia w ruchu" : "Srednia (w ruchu)", 1, 0, y0);
    s_moving = make_value(t, "Czas jazdy", 0, 1, y0);
    s_now = make_value(t, "Predkosc", 1, 1, y0);

    s_sd = make_label(t, &lv_font_montserrat_14, k_dim, 0, 0, "");
    lv_obj_align(s_sd, LV_ALIGN_BOTTOM_MID, 0, -44);

    lv_obj_t *btn = lv_button_create(t);
    lv_obj_set_size(btn, 220, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8A2E2E), LV_STATE_PRESSED);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Zeruj trip (przytrzymaj)");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, on_reset_trip, LV_EVENT_LONG_PRESSED, NULL);

    s_reset_note = make_label(t, &lv_font_montserrat_20, 0x4CD97B, 0, 0, "Trip wyzerowany");
    lv_obj_align(s_reset_note, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(s_reset_note, LV_OBJ_FLAG_HIDDEN);
}

// ---- whole UI (rebuilt on a rotation) --------------------------------------------------------
static lv_obj_t *s_tv;

static void build_ui()
{
    lv_display_t *d = lv_display_get_default();
    s_w = lv_display_get_horizontal_resolution(d);
    s_h = lv_display_get_vertical_resolution(d);
    s_port = s_h > s_w;
    s_tv = lv_tileview_create(lv_screen_active());
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(k_bg), 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    build_speedo_screen(lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT));
    build_accel_screen(lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR));
    build_odo_screen(lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_HOR));
    build_gps_screen(lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_HOR));
    build_wifi_screen(lv_tileview_add_tile(s_tv, 4, 0, LV_DIR_LEFT));
}

static uint8_t load_rotation()
{
    Preferences p;
    uint8_t r = 0;
    if (p.begin("cyd", true)) {
        r = p.getUChar("rot", 0) & 3;
        p.end();
    }
    return r;
}

static void apply_rotation(uint8_t rot)
{
    rot &= 3;
    if (rot == cyd_rotation()) {
        return;
    }
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tv);
    const uint32_t idx = tile ? lv_obj_get_index(tile) : 0;
    qr_close();
    lv_timer_delete(s_acc_timer);
    s_acc_timer = NULL;
    lv_obj_delete(s_tv);
    cyd_set_rotation(rot);
    build_ui();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);  // stay on the same screen
    refresh(NULL);
    Preferences p;
    if (p.begin("cyd", false)) {
        p.putUChar("rot", rot);
        p.end();
    }
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("ROT: %u deg, %dx%d, LVGL heap %lu B free, biggest %lu B\n", rot * 90u, s_w, s_h,
                  (unsigned long)m.free_size, (unsigned long)m.free_biggest_size);
}

static void calib_done(const cyd_cal_t *cal, bool ok)
{
    if (!ok) {
        Serial.println("calibration unusable, again");
        cyd_calib_ui_start(lv_display_get_default(), cyd_touch_raw, calib_done);
        return;
    }
    cyd_touch_set_cal(cal);
    Serial.printf("calibration %s\n", cyd_cal_save(cal) ? "saved" : "NOT saved");
    s_rot_req = load_rotation();  // calibration always runs in landscape; now turn the UI back
}

// Show a hint and give the user 2 s to press BOOT (GPIO0) to force touch calibration.
static bool boot_pressed_after_start(lv_obj_t *scr)
{
    lv_obj_t *hint = make_label(scr, &lv_font_montserrat_14, 0xFFC145, 0, 0, "BOOT = kalibracja dotyku");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    lv_refr_now(NULL);
    cyd_backlight(100);
    bool pressed = false;
    for (uint32_t t0 = millis(); millis() - t0 < 2000 && !pressed;) {
        pressed = digitalRead(0) == LOW;
        delay(20);
    }
    lv_obj_delete(hint);
    return pressed;
}

void setup()
{
    Serial.begin(115200);
    bootlog_begin();
    pinMode(0, INPUT_PULLUP);  // BOOT
    gps_begin();
    odometer_begin();
    accel_begin();
    gpx_log_begin();
    if (!cyd_begin()) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(k_bg), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    cyd_cal_t cal;
    bool have = cyd_cal_load(&cal);
    if (have && boot_pressed_after_start(scr)) {
        have = false;
    }
    if (have) {
        cyd_touch_set_cal(&cal);
        cyd_set_rotation(load_rotation());  // without a calibration: landscape until it is done
    }
    build_ui();

    net_begin();  // after the display: the Wi-Fi start-up current peak comes with the backlight already on
    refresh(NULL);
    lv_timer_create(refresh, 500, NULL);
    if (!have) {
        cyd_calib_ui_start(lv_display_get_default(), cyd_touch_raw, calib_done);
    }
    lv_refr_now(NULL);
    cyd_backlight(100);
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("LVGL heap: %u%% used, %lu B free, biggest block %lu B\n", m.used_pct, (unsigned long)m.free_size,
                  (unsigned long)m.free_biggest_size);
    Serial.printf("heap: %u B free, min %u B, largest block %u B\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

#ifdef BOOTLOG_WDT_TEST
// Test build only (env cyd2usb_wdttest): block CPU 0 so the task watchdog must fire and the
// restart log must name this task. Never in a release.
static void wdt_hog(void *)
{
    Serial.println("WDT TEST: blocking CPU 0");
    for (uint32_t t0 = millis(); millis() - t0 < 8000;) {
    }
    vTaskDelete(NULL);
}
#endif

// Single-letter commands on the USB serial:
//   n  toggle raw NMEA echo      T  zero the whole odometer (trip and total)
// USB serial (115200): n = NMEA echo on/off, T = zero the whole odometer,
// L = list GPX files, D <name> = delete a GPX file (D alone lists), R = rotate screen,
// B = restart log (reasons of the last starts), ? = help.
static void serial_commands()
{
    while (Serial.available()) {
        switch (Serial.read()) {
        case 'n':
            gps_toggle_echo();
            break;
        case 'T':
            odometer_reset_all();
            break;
        case 'L':
            gpx_log_print_files();
            break;
        case 'B':
            bootlog_print();
            break;
#ifdef BOOTLOG_WDT_TEST
        case 'W':
            xTaskCreatePinnedToCore(wdt_hog, "wdt_hog", 2048, NULL, 1, NULL, 0);
            break;
#endif
        case 'R':
            s_rot_req = (cyd_rotation() + 1) & 3;  // turn the screen by 90 degrees
            break;
        case 'D': {
            String name = Serial.readStringUntil('\n');  // rest of the line, 1 s timeout
            name.trim();
            if (name.isEmpty()) {
                gpx_log_print_files();
                Serial.println("DEL: usage: D 20261123-114602.gpx");
            } else {
                Serial.printf("DEL: %s: %s\n", name.c_str(), gpx_log_delete(name.c_str()));
            }
            break;
        }
        case '?':
            Serial.println("CMD: n NMEA echo, T zero odometer, L list GPX, D <name> delete GPX, R rotate screen, B restart log");
            break;
        }
    }
}

void loop()
{
#ifdef ROLLBACK_TEST
    // Deliberately broken release: crashes before the 15 s health check confirms it, so the
    // bootloader must fall back to the previous firmware. Never publish this build.
    static bool warned;
    if (!warned) {
        warned = true;
        lv_obj_t *w = make_label(lv_layer_top(), &lv_font_montserrat_20, 0xFF5A5A, 0, 0, "TEST ROLLBACK: crash za 8 s");
        lv_obj_align(w, LV_ALIGN_CENTER, 0, 0);
        Serial.println("ROLLBACK TEST: running " FW_VERSION ", crashing at 8 s");
    }
    if (millis() > 8000) {
        Serial.println("ROLLBACK TEST: abort()");
        Serial.flush();
        abort();
    }
#endif
    bootlog_tick();
    serial_commands();
    gps_poll();
    net_poll();
    static uint32_t last_seq;
    if (gps_fix_seq() != last_seq) {
        last_seq = gps_fix_seq();
        gps_data_t g;
        gps_get(&g);
        odometer_feed(&g);
        gpx_log_feed(&g);
    }
    static uint32_t last_epoch;
    if (gps_spd_seq() != last_epoch) {  // one speed sample per GPS epoch (10 Hz)
        last_epoch = gps_spd_seq();
        gps_data_t g;
        gps_get(&g);
        accel_feed(&g);
    }
    gpx_log_poll();
    if (web_take_clear_runs()) {
        accel_clear_history();
    }
    const int web_rot = web_take_rotation();
    if (web_rot >= 0) {
        s_rot_req = web_rot;
    }
    if (s_rot_req >= 0) {
        const int r = s_rot_req;
        s_rot_req = -1;
        apply_rotation((uint8_t)r);
    }
    cyd_loop();
}
