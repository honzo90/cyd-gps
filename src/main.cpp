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

// ---- look: one palette, shared styles (a style shared by many objects costs no LVGL heap per object)
static const uint32_t k_bg = 0x0E1216;       // screen
static const uint32_t k_surface = 0x182029;  // cards
static const uint32_t k_raised = 0x26303B;   // buttons
static const uint32_t k_pressed = 0x37434F;
static const uint32_t k_text = 0xF2F5F7;     // values
static const uint32_t k_text2 = 0xC5CDD4;    // longer text
static const uint32_t k_caption = 0x8C97A1;  // captions (>= 4.5:1 on the cards)
static const uint32_t k_dim = 0x6B7680;      // hints only
static const uint32_t k_accent = 0xFF7A1A;   // running clock, records
static const uint32_t k_ok = 0x4CD97B, k_warn = 0xFFC145, k_err = 0xFF5A5A;

static lv_style_t s_st_card, s_st_btn, s_st_btn_pr, s_st_pill;

static void styles_init()
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    lv_style_init(&s_st_card);
    lv_style_set_bg_color(&s_st_card, lv_color_hex(k_surface));
    lv_style_set_bg_opa(&s_st_card, LV_OPA_COVER);
    lv_style_set_radius(&s_st_card, 10);
    lv_style_init(&s_st_btn);
    lv_style_set_bg_color(&s_st_btn, lv_color_hex(k_raised));
    lv_style_set_radius(&s_st_btn, 10);
    lv_style_set_shadow_width(&s_st_btn, 0);
    lv_style_set_text_color(&s_st_btn, lv_color_hex(k_text));
    lv_style_init(&s_st_btn_pr);
    lv_style_set_bg_color(&s_st_btn_pr, lv_color_hex(k_pressed));
    lv_style_set_translate_y(&s_st_btn_pr, 1);
    lv_style_init(&s_st_pill);
    lv_style_set_radius(&s_st_pill, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_st_pill, LV_OPA_COVER);
    lv_style_set_pad_hor(&s_st_pill, 10);
    lv_style_set_pad_ver(&s_st_pill, 3);
}

// Rounded surface behind a group of values. Created before them, so it is drawn under them.
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_add_style(c, &s_st_card, 0);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_remove_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return c;
}

// Every button: same shape, same pressed state. The label is child 0.
static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &s_st_btn, 0);
    lv_obj_add_style(b, &s_st_btn_pr, LV_STATE_PRESSED);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}


// Layout of the current rotation (see cyd_set_rotation): 320 x 240 or 240 x 320.
static int s_w = 320, s_h = 240;
static bool s_port;  // portrait: the status goes on its own line under the title
static int dy() { return s_port ? 26 : 0; }

// Speedometer
static seg7_t s_seg;
static lv_obj_t *s_spd_bar, *s_spd_scale, *s_spd_info, *s_dir_arrow, *s_dir_text;
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
static lv_obj_t *s_lat, *s_lon, *s_alt, *s_speed, *s_course, *s_sats, *s_hdop, *s_time, *s_diag;
// Odometer screen
static lv_obj_t *s_trip, *s_total, *s_max, *s_avg, *s_moving, *s_now, *s_reset_note, *s_sd;

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
    return make_label(parent, &lv_font_montserrat_20, k_text, x, y + 16, "--");
}

static lv_obj_t *s_tv;  // the screens (tileview)

// Height of the bottom navigation bar; the screens get the rest (s_ch).
static const int k_nav_h = 38;
static int s_ch = 240 - k_nav_h;
static uint32_t s_gps_col = 0x8C97A1;  // GPS status colour, shown on the GPS icon of the bar
static void nav_update();

// Screen status pill (POMIAR: GOTOWY / POMIAR / ...).
static lv_obj_t *make_pill(lv_obj_t *t, int x, int y)
{
    lv_obj_t *l = make_label(t, &lv_font_montserrat_14, k_text, x, y, "");
    lv_obj_add_style(l, &s_st_pill, 0);
    return l;
}

static void set_pill(lv_obj_t *o, const char *text, uint32_t color)
{
    lv_label_set_text(o, text);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(o, lv_color_mix(lv_color_hex(color), lv_color_hex(k_bg), 56), 0);  // ~22 % tint
}

// GPS state: colour of the GPS icon in the bar (always visible) and the RGB LED.
static void set_status(const char *, uint32_t color, bool r, bool g)
{
    if (color != s_gps_col) {
        s_gps_col = color;
        nav_update();
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
        lv_label_set_text_fmt(s_diag, "Odb. %u/%u  >=30 dB: %u  top4: %u  %.0f Hz", g.sat_tracked,
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
    snprintf(buf, sizeof buf, o->total_m < 100000 ? "Calkowity  %.2f km" : "Calkowity  %.0f km", o->total_m / 1000.0);
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
    snprintf(buf, sizeof buf, s_port ? "Trip %.2f km   Max %.0f   Sat %u" : "Trip %.2f km     Max %.0f     Sat %u",
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
        set_status("BRAK DANYCH", k_err, true, false);
    } else if (!g.baud_locked) {
        set_status("SZUKAM BAUD", k_err, true, false);
    } else if (!g.fix) {
        set_status("SZUKAM FIX", k_warn, true, true);
    } else {
        set_status("FIX", k_ok, false, true);
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
        col = k_err;
    } else if (a.rate_hz > 0 && a.rate_hz < 5) {
        st = "GPS 1 Hz";
        col = k_warn;
    } else if (a.phase == ACC_RUNNING) {
        st = "POMIAR";
        col = k_accent;
    } else if (a.phase == ACC_ARMED) {
        st = "GOTOWY";
        col = k_ok;
    } else {
        st = "ZATRZYMAJ";  // stand still 1 s to arm
        col = k_text2;
    }
    set_pill(s_acc_status, st, col);

    char b[24];
    fmt_time(b, sizeof b, a.phase == ACC_RUNNING || a.has_last ? a.elapsed_s : 0);
    lv_label_set_text(s_acc_clock, b);
    lv_obj_set_style_text_color(s_acc_clock, lv_color_hex(a.phase == ACC_RUNNING ? k_accent : k_text), 0);
    lv_label_set_text_fmt(s_acc_speed, "%d km/h %.0fHz", (int)(g.spd_kmh + 0.5f), a.rate_hz);

    for (int i = 0; i < ACC_N; ++i) {
        fmt_time(b, sizeof b, a.has_last ? a.last.t[i] : -1);
        lv_label_set_text(s_acc_val[i], b);
        // this run set the record: the value in the accent colour
        const bool record = a.has_last && a.phase != ACC_RUNNING && a.last.t[i] > 0 && a.best[i] > 0 &&
                            fabsf(a.last.t[i] - a.best[i]) < 0.005f;
        lv_obj_set_style_text_color(s_acc_val[i], lv_color_hex(record ? k_accent : k_text), 0);
        if (a.best[i] > 0) {
            lv_label_set_text_fmt(s_acc_best[i], s_port ? "%.2f" : "naj. %.2f", a.best[i]);
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
            snprintf(line, sizeof line, "Vmax %.0f km/h\n%.0f m%s%s", a.last.v_max, a.last.dist_m, g[0] ? "\n" : "", g);
        }
        lv_label_set_text(s_acc_note, line);
    } else if (a.phase == ACC_ARMED && !a.stats.runs) {
        lv_label_set_text(s_acc_note, "Ruszaj - czas startuje sam");
    } else if (a.stats.runs) {  // short statistics; the full history is on the web page
        const float m = acc_stats_mean(&a.stats, ACC_T_0_100);
        if (m > 0) {
            char line[64];
            snprintf(line, sizeof line, s_port ? "Pomiary: %lu, sr. 0-100 %.2f s  " LV_SYMBOL_RIGHT
                                               : "Pomiary: %lu  " LV_SYMBOL_RIGHT "\nsr. 0-100 %.2f s",
                     (unsigned long)a.stats.runs, m);
            lv_label_set_text(s_acc_note, line);
        } else {
            lv_label_set_text_fmt(s_acc_note, "Pomiary: %lu  " LV_SYMBOL_RIGHT, (unsigned long)a.stats.runs);
        }
    } else {
        lv_label_set_text(s_acc_note, "Tylko na zamknietym torze.");
    }
}

// ---- list of the last runs (overlay, built on tap, deleted on close: no RAM while hidden) ----
static lv_obj_t *s_runs_panel;

static void runs_close()
{
    if (s_runs_panel) {
        lv_obj_delete_async(s_runs_panel);  // may be called from the panel's own event
        s_runs_panel = NULL;
    }
}

static void on_runs_close(lv_event_t *)
{
    runs_close();
}

static void on_runs_open(lv_event_t *)
{
    if (s_runs_panel) {
        return;
    }
    static acc_hist_row_t rows[8];
    const int max = s_port ? 8 : 6;
    const int n = accel_recent(rows, max);
    accel_view_t a;
    accel_get(&a);

    s_runs_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_runs_panel);
    lv_obj_set_size(s_runs_panel, s_w, s_h);
    lv_obj_set_style_bg_color(s_runs_panel, lv_color_hex(k_bg), 0);
    lv_obj_set_style_bg_opa(s_runs_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_runs_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_runs_panel, on_runs_close, LV_EVENT_CLICKED, NULL);  // tap anywhere closes

    make_label(s_runs_panel, &lv_font_montserrat_20, 0xFFFFFF, 12, 8, "Ostatnie pomiary");
    if (!n) {
        make_label(s_runs_panel, &lv_font_montserrat_14, 0x9AA4AE, 12, 48,
                   gpx_log_sd_ok() ? "Brak zapisanych pomiarow." : "Brak karty SD.");
    } else {
        // columns: one label each, rows separated by '\n' (same line height keeps them aligned)
        const int nc = s_port ? 4 : 5;
        static const int xl[5] = {8, 100, 160, 222, 262}, xp[4] = {8, 98, 150, 200};
        static const char *const head[5] = {"kiedy", "0-100 s", "1/4 mili", "Vmax", ""};
        static char col[5][8 * 16 + 1];
        for (int c = 0; c < nc; ++c) {
            col[c][0] = 0;
        }
        for (int i = 0; i < n; ++i) {
            const acc_hist_row_t &r = rows[i];
            char cell[5][16];
            acc_hist_when(r.id, cell[0], sizeof cell[0]);
            for (int c = 1; c <= 2; ++c) {  // seconds are in the header
                const float t = r.t[c == 1 ? ACC_T_0_100 : ACC_T_QUARTER];
                snprintf(cell[c], sizeof cell[c], t > 0 ? "%.2f" : "--", t);
            }
            snprintf(cell[3], sizeof cell[3], "%.0f", r.vmax);
            if (r.aborted) {
                snprintf(cell[4], sizeof cell[4], s_port ? "prz." : "przerw.");
            } else if (r.low_rate) {
                snprintf(cell[4], sizeof cell[4], "1 Hz");
            } else if (!isnan(r.grade_pct)) {
                snprintf(cell[4], sizeof cell[4], "%+.0f%%", r.grade_pct);
            } else {
                cell[4][0] = 0;
            }
            if (s_port) {  // portrait: no Vmax, the note takes its place
                memcpy(cell[3], cell[4], sizeof cell[3]);
            }
            for (int c = 0; c < nc; ++c) {
                char *dst = col[c];
                const size_t l = strlen(dst);
                snprintf(dst + l, sizeof col[c] - l, "%s%s", i ? "\n" : "", cell[c]);
            }
        }
        const int y0 = 40;
        for (int c = 0; c < nc; ++c) {
            const int x = s_port ? xp[c] : xl[c];
            const char *h = s_port && c == 3 ? "" : head[c];
            make_label(s_runs_panel, &lv_font_montserrat_14, k_caption, x, y0, h);
            lv_obj_t *l = make_label(s_runs_panel, &lv_font_montserrat_14, c == 1 ? 0xFFFFFF : 0xC8D0D8, x, y0 + 20,
                                     col[c]);
            lv_obj_set_style_text_line_space(l, 2, 0);
        }
    }
    // summary in two short lines left of / above the button: count + best, mean and spread of 0-100
    char sum[96];
    const float m = acc_stats_mean(&a.stats, ACC_T_0_100), sd = acc_stats_sd(&a.stats, ACC_T_0_100);
    int k = snprintf(sum, sizeof sum, "Pomiary: %lu", (unsigned long)a.stats.runs);
    if (a.best[ACC_T_0_100] > 0) {
        k += snprintf(sum + k, sizeof sum - k, ", naj. %.2f s", a.best[ACC_T_0_100]);
    }
    if (m > 0) {
        k += snprintf(sum + k, sizeof sum - k, "\nsr. 0-100 %.2f s", m);
        if (sd >= 0) {
            snprintf(sum + k, sizeof sum - k, " (+-%.2f)", sd);
        }
    }
    lv_obj_t *sl = make_label(s_runs_panel, &lv_font_montserrat_14, 0x9AA4AE, 12, 0, sum);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 12, s_port ? -46 : -6);
    lv_obj_t *close = lv_button_create(s_runs_panel);
    lv_obj_set_size(close, 96, 34);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x2A3138), 0);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Zamknij");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, on_runs_close, LV_EVENT_CLICKED, NULL);
}

// Restart feedback (~0.5 s): the ⟳ icon turns once, the clock and the results fade back in.
static lv_obj_t *s_acc_rst_icon;

static void anim_rotate(void *obj, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0);
}

static void anim_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void restart_anim()
{
    lv_anim_t a;
    if (s_acc_rst_icon) {
        lv_obj_update_layout(s_acc_rst_icon);
        lv_obj_set_style_transform_pivot_x(s_acc_rst_icon, lv_obj_get_width(s_acc_rst_icon) / 2, 0);
        lv_obj_set_style_transform_pivot_y(s_acc_rst_icon, lv_obj_get_height(s_acc_rst_icon) / 2, 0);
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_acc_rst_icon);
        lv_anim_set_exec_cb(&a, anim_rotate);
        lv_anim_set_values(&a, 0, 3600);  // 0.1 degree units: one full turn
        lv_anim_set_duration(&a, 500);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    lv_obj_t *fade[1 + ACC_N] = {s_acc_clock};
    memcpy(fade + 1, s_acc_val, sizeof s_acc_val);
    for (int i = 0; i < 1 + ACC_N; ++i) {
        lv_anim_init(&a);
        lv_anim_set_var(&a, fade[i]);
        lv_anim_set_exec_cb(&a, anim_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 350);
        lv_anim_set_delay(&a, 30 * i);  // top to bottom, like a wipe
        lv_anim_set_early_apply(&a, true);  // invisible at once, also during the delay
        lv_anim_start(&a);
    }
}

static void on_acc_restart(lv_event_t *)
{
    accel_restart();
    refresh_accel(NULL);
    restart_anim();
}

static void on_acc_reset(lv_event_t *)
{
    accel_reset_best();
    refresh_accel(NULL);
}

static void build_accel_screen(lv_obj_t *t)
{
    // top row: status pill, speed + GPS rate; icon buttons (list, restart) on the right
    s_acc_status = make_pill(t, 10, 8);
    s_acc_speed = make_label(t, &lv_font_montserrat_14, k_caption, s_port ? 12 : 120, s_port ? 40 : 11, "");
    for (int b = 0; b < 2; ++b) {
        lv_obj_t *btn = make_button(t, b ? LV_SYMBOL_REFRESH : LV_SYMBOL_LIST, 46, 32);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8 - (b ? 0 : 52), 4);
        lv_obj_add_event_cb(btn, b ? on_acc_restart : on_runs_open, LV_EVENT_CLICKED, NULL);
        if (b) {
            s_acc_rst_icon = lv_obj_get_child(btn, 0);
        }
    }
    // the clock, large; long press clears the best times (rare, so no extra button)
    s_acc_clock = make_label(t, &lv_font_montserrat_48, k_text, 10, s_port ? 52 : 34, "0.00 s");
    lv_obj_add_flag(s_acc_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_acc_clock, on_acc_reset, LV_EVENT_LONG_PRESSED, NULL);

    // note (result summary / hint / run count): tap = list of the last runs
    s_acc_note = make_label(t, &lv_font_montserrat_14, k_caption, 0, 0, "");
    if (s_port) {
        lv_obj_set_width(s_acc_note, s_w - 20);
        lv_label_set_long_mode(s_acc_note, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_acc_note, 12, 102);
    } else {  // a narrow column right of the clock, up to 3 lines
        lv_obj_set_size(s_acc_note, 122, 52);
        lv_label_set_long_mode(s_acc_note, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_acc_note, s_w - 130, 40);
    }
    lv_obj_add_flag(s_acc_note, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_acc_note, 10);
    lv_obj_add_event_cb(s_acc_note, on_runs_open, LV_EVENT_CLICKED, NULL);

    if (s_port) {  // table: name | last | best, with a small column header
        const int y0 = 120, rh = 23;
        make_card(t, 6, y0, s_w - 12, 18 + ACC_N * rh + 4);
        lv_obj_t *h1 = make_label(t, &lv_font_montserrat_14, k_dim, 120, y0 + 2, "ostatni");
        lv_obj_t *h2 = make_label(t, &lv_font_montserrat_14, k_dim, 0, 0, "naj.");
        lv_obj_align(h2, LV_ALIGN_TOP_RIGHT, -14, y0 + 2);
        (void)h1;
        for (int i = 0; i < ACC_N; ++i) {
            const int y = y0 + 18 + i * rh;
            make_label(t, &lv_font_montserrat_14, k_caption, 14, y + 4, k_acc_names[i]);
            s_acc_val[i] = make_label(t, &lv_font_montserrat_20, k_text, 120, y, "--");
            s_acc_best[i] = make_label(t, &lv_font_montserrat_14, k_caption, 0, y + 4, "");
            lv_obj_align(s_acc_best[i], LV_ALIGN_TOP_RIGHT, -14, y + 4);
        }
    } else {  // 2 columns x 3 rows: name, value, best beside the value
        const int y0 = 88, rh = 38;
        make_card(t, 6, y0, s_w - 12, 3 * rh + 2);
        for (int i = 0; i < ACC_N; ++i) {
            const int x = 16 + (i / 3) * 152, y = y0 + 2 + (i % 3) * rh;
            make_label(t, &lv_font_montserrat_14, k_caption, x, y + 2, k_acc_names[i]);
            s_acc_val[i] = make_label(t, &lv_font_montserrat_20, k_text, x, y + 16, "--");
            s_acc_best[i] = make_label(t, &lv_font_montserrat_14, k_caption, x + 66, y + 20, "");
            lv_obj_set_width(s_acc_best[i], 72);
            lv_obj_set_style_text_align(s_acc_best[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    // 100 ms: the clock runs smoothly while the rest of the UI refreshes every 500 ms
    s_acc_timer = lv_timer_create(refresh_accel, 100, NULL);
}

static void build_speedo_screen(lv_obj_t *t)
{
    // trip / max / satellites on top; the digits are the hero
    s_spd_info = make_label(t, &lv_font_montserrat_14, k_caption, 12, 8, "");

    // Landscape: 3 digits of 72 x 116 px, 15 px stroke: 3*72 + 2*14 = 244 px wide, centred.
    // Portrait: 66 x 120 px, 14 px stroke: 3*66 + 2*10 = 218 px in the 240 px width.
    const int dw = s_port ? 218 : 244, mx = (s_w - dw) / 2;  // digits / bar width, left margin
    if (s_port) {
        seg7_create(&s_seg, t, mx, 36, 66, 120, 14, 10);
    } else {
        seg7_create(&s_seg, t, mx, 32, 72, 116, 15, 14);
    }
    const int y_scale = s_port ? 164 : 152;  // row under the digits: scale, (direction,) unit

    lv_obj_t *unit = make_label(t, &lv_font_montserrat_20, k_caption, 0, 0, "km/h");
    lv_obj_align(unit, LV_ALIGN_TOP_RIGHT, -mx, y_scale);
    s_spd_scale = make_label(t, &lv_font_montserrat_14, k_dim, mx, y_scale + 4, "");

    s_spd_bar = lv_bar_create(t);
    lv_obj_set_size(s_spd_bar, dw, 10);
    lv_obj_set_pos(s_spd_bar, mx, y_scale + 28);
    lv_obj_set_style_bg_color(s_spd_bar, lv_color_hex(k_surface), 0);
    lv_obj_set_style_bg_color(s_spd_bar, lv_color_hex(k_ok), LV_PART_INDICATOR);
    lv_obj_remove_flag(s_spd_bar, LV_OBJ_FLAG_CLICKABLE);

    // direction of travel (GPS course, shown from 5 km/h): arrow + "NE 47°"
    // landscape: between scale and unit; portrait: own row under the bar
    s_dir_arrow = lv_line_create(t);
    lv_obj_set_size(s_dir_arrow, 24, 24);
    lv_obj_set_pos(s_dir_arrow, s_port ? 72 : 110, s_port ? 214 : y_scale - 3);
    lv_obj_set_style_line_width(s_dir_arrow, 3, 0);
    lv_obj_set_style_line_color(s_dir_arrow, lv_color_hex(k_ok), 0);
    lv_obj_set_style_line_rounded(s_dir_arrow, true, 0);
    set_arrow(0);
    lv_obj_add_flag(s_dir_arrow, LV_OBJ_FLAG_HIDDEN);
    s_dir_text = make_label(t, &lv_font_montserrat_20, k_text, s_port ? 102 : 140, s_port ? 215 : y_scale - 2, "--");
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
    // the card clips the text: a long hotspot description never runs under the buttons
    const int ch = s_port ? 164 : 126;
    lv_obj_t *card = make_card(t, 6, 6, s_w - 12, ch);
    s_wifi_info = make_label(card, &lv_font_montserrat_14, k_text2, 8, 6, "");
    lv_obj_set_width(s_wifi_info, s_w - 12 - 16);
    lv_obj_set_style_text_line_space(s_wifi_info, 3, 0);

    // Buttons. Landscape: [update | QR] / [Wi-Fi on/off | rotate]. Portrait: update, [QR | rotate], Wi-Fi.
    const int bh = 30, y1 = ch + 12, y2 = y1 + bh + 4, y3 = y2 + bh + 4;
    const int wide = s_port ? s_w - 16 : 200, small = s_port ? (s_w - 20) / 2 : s_w - 16 - 200 - 4;

    // Install from GitHub: long press, so a stray touch while driving cannot start it.
    s_gh_btn = make_button(t, "", wide, bh);
    lv_obj_set_pos(s_gh_btn, 8, y1);
    lv_obj_set_style_bg_color(s_gh_btn, lv_color_hex(0x1F5A36), 0);
    s_gh_btn_label = lv_obj_get_child(s_gh_btn, 0);
    lv_obj_add_event_cb(s_gh_btn, on_gh_install, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_gh_btn, LV_OBJ_FLAG_HIDDEN);

    // Check for updates: a plain click (it only asks GitHub, installs nothing).
    s_chk_btn = make_button(t, "", wide, bh);
    lv_obj_set_pos(s_chk_btn, 8, y1);
    s_chk_btn_label = lv_obj_get_child(s_chk_btn, 0);
    lv_obj_add_event_cb(s_chk_btn, on_gh_check, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_chk_btn, LV_OBJ_FLAG_HIDDEN);

    // QR login: long press, so the password and code are not shown by a stray touch.
    lv_obj_t *qr = make_button(t, "QR", small, bh);
    lv_obj_set_pos(qr, s_port ? 8 : 8 + wide + 4, s_port ? y2 : y1);
    lv_obj_add_event_cb(qr, on_qr_open, LV_EVENT_LONG_PRESSED, NULL);

    // Rotate the screen by 90 degrees per tap (remembered; also on the web page).
    lv_obj_t *rot = make_button(t, LV_SYMBOL_REFRESH " Obroc", small, bh);
    lv_obj_set_pos(rot, s_port ? 12 + small : 8 + wide + 4, y2);
    lv_obj_add_event_cb(rot, on_rotate, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn = make_button(t, "", wide, bh);
    lv_obj_set_pos(btn, 8, s_port ? y3 : y2);
    s_wifi_btn_label = lv_obj_get_child(btn, 0);
    lv_obj_add_event_cb(btn, on_wifi_toggle, LV_EVENT_CLICKED, NULL);
}

static void build_gps_screen(lv_obj_t *t)
{
    // portrait: latitude and longitude need a full row each, the rest is shifted down by one row
    const int y0 = 10, r = s_port ? 1 : 0;
    make_card(t, 6, y0 - 4, s_w - 12, (4 + r) * 44 - 2);
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
    lv_obj_align(s_diag, LV_ALIGN_TOP_MID, 0, y0 + (4 + r) * 44);
}

static void build_odo_screen(lv_obj_t *t)
{
    // trip is the hero; total beside it (landscape) or under it (portrait)
    make_label(t, &lv_font_montserrat_14, k_caption, 12, 6, "Trip");
    s_trip = make_label(t, &lv_font_montserrat_48, k_text, 10, 20, "--");
    s_total = make_label(t, &lv_font_montserrat_14, k_text2, 0, 0, "--");  // "Calkowity 1287 km", top right
    lv_obj_align(s_total, LV_ALIGN_TOP_RIGHT, -12, 6);

    const int y0 = s_port ? 86 : 82;
    make_card(t, 6, y0 - 4, s_w - 12, 2 * 44 - 2);
    s_max = make_value(t, "Maks. predkosc", 0, 0, y0);
    s_avg = make_value(t, s_port ? "Sr. w ruchu" : "Srednia (w ruchu)", 1, 0, y0);
    s_moving = make_value(t, "Czas jazdy", 0, 1, y0);
    s_now = make_value(t, "Predkosc", 1, 1, y0);

    // bottom: SD state + trip reset (long press)
    const int yb = y0 + 2 * 44 + 2;
    const int bw = s_port ? s_w - 16 : 156;
    lv_obj_t *btn = make_button(t, s_port ? "Zeruj trip (przytrzymaj)" : "Zeruj (przytrzymaj)", bw, 30);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8A2E2E), LV_STATE_PRESSED);  // destructive
    lv_obj_add_event_cb(btn, on_reset_trip, LV_EVENT_LONG_PRESSED, NULL);
    s_sd = make_label(t, &lv_font_montserrat_14, k_dim, 12, 0, "");
    lv_label_set_long_mode(s_sd, LV_LABEL_LONG_DOT);
    if (s_port) {
        lv_obj_set_width(s_sd, s_w - 24);
        lv_obj_set_y(s_sd, yb + 4);
        lv_obj_set_pos(btn, 8, yb + 28);
    } else {
        lv_obj_set_width(s_sd, s_w - bw - 8 - 24);
        lv_obj_set_y(s_sd, yb + 8);
        lv_obj_set_pos(btn, s_w - 8 - bw, yb + 2);
    }

    s_reset_note = make_label(t, &lv_font_montserrat_20, k_ok, 0, 0, "Trip wyzerowany");
    lv_obj_align(s_reset_note, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_reset_note, LV_OBJ_FLAG_HIDDEN);
}

// ---- whole UI (rebuilt on a rotation) --------------------------------------------------------

// Bottom navigation: one item per screen. The active one shows its name on an accent pill; the
// GPS icon always carries the GPS state colour. Swiping still works; the bar follows it.
static lv_obj_t *s_nav, *s_nav_item[5];
static const char *const k_nav_icon[5] = {LV_SYMBOL_HOME, LV_SYMBOL_CHARGE, LV_SYMBOL_LOOP, LV_SYMBOL_GPS,
                                          LV_SYMBOL_WIFI};
static const char *const k_nav_name[5] = {"Predkosc", "Pomiar", "Licznik", "GPS", "Wi-Fi"};

static void nav_update()
{
    if (!s_nav || !s_tv) {
        return;
    }
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tv);
    const int active = tile ? (int)lv_obj_get_index(tile) : 0;
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *it = s_nav_item[i], *l = lv_obj_get_child(it, 0);
        const bool on = i == active;
        if (on) {
            char b[32];
            snprintf(b, sizeof b, "%s  %s", k_nav_icon[i], k_nav_name[i]);
            lv_label_set_text(l, b);
        } else {
            lv_label_set_text(l, k_nav_icon[i]);
        }
        const uint32_t fg = i == 3 ? s_gps_col : on ? k_accent : k_caption;
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_obj_set_style_bg_opa(it, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void on_nav(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_tileview_set_tile_by_index(s_tv, i, 0, LV_ANIM_ON);
    nav_update();
}

static void on_tile_changed(lv_event_t *)
{
    nav_update();
}

static void build_nav()
{
    s_nav = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_nav);
    lv_obj_set_size(s_nav, s_w, k_nav_h);
    lv_obj_set_pos(s_nav, 0, s_ch);
    lv_obj_set_style_bg_color(s_nav, lv_color_hex(k_surface), 0);
    lv_obj_set_style_bg_opa(s_nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_nav, 1, 0);
    lv_obj_set_style_border_color(s_nav, lv_color_hex(k_raised), 0);
    lv_obj_set_flex_flow(s_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_nav, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *it = lv_obj_create(s_nav);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, LV_SIZE_CONTENT, 28);
        lv_obj_set_style_pad_hor(it, s_port ? 8 : 12, 0);
        lv_obj_set_style_radius(it, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(it, lv_color_mix(lv_color_hex(k_accent), lv_color_hex(k_surface), 48), 0);
        lv_obj_set_ext_click_area(it, 6);
        lv_obj_remove_flag(it, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(it);
        lv_obj_center(l);
        lv_obj_add_event_cb(it, on_nav, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_nav_item[i] = it;
    }
}

static void build_ui()
{
    styles_init();
    lv_display_t *d = lv_display_get_default();
    s_w = lv_display_get_horizontal_resolution(d);
    s_h = lv_display_get_vertical_resolution(d);
    s_port = s_h > s_w;
    s_ch = s_h - k_nav_h;
    s_tv = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(s_tv, s_w, s_ch);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(k_bg), 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    build_speedo_screen(lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT));
    build_accel_screen(lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR));
    build_odo_screen(lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_HOR));
    build_gps_screen(lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_HOR));
    build_wifi_screen(lv_tileview_add_tile(s_tv, 4, 0, LV_DIR_LEFT));
    lv_obj_add_event_cb(s_tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);
    build_nav();
    nav_update();
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
    if (s_runs_panel) {
        lv_obj_delete(s_runs_panel);
        s_runs_panel = NULL;
    }
    lv_timer_delete(s_acc_timer);
    s_acc_timer = NULL;
    lv_obj_delete(s_tv);
    lv_obj_delete(s_nav);
    s_tv = s_nav = NULL;
    cyd_set_rotation(rot);
    build_ui();
    lv_tileview_set_tile_by_index(s_tv, idx, 0, LV_ANIM_OFF);  // stay on the same screen
    nav_update();
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

// Start screen: icon, name and a bar filling over the 2 s in which BOOT (GPIO0) forces the touch
// calibration. Nothing technical on it (the BOOT trick is in the notes). Deleted afterwards.
static lv_obj_t *splash_create(lv_obj_t *scr, lv_obj_t **bar)
{
    styles_init();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(k_bg), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_t *icon = make_label(p, &lv_font_montserrat_48, k_accent, 0, 0, LV_SYMBOL_GPS);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -44);
    lv_obj_t *name = make_label(p, &lv_font_montserrat_32, k_text, 0, 0, "CYD GPS");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 14);
    *bar = lv_bar_create(p);
    lv_obj_set_size(*bar, 140, 6);
    lv_obj_align(*bar, LV_ALIGN_CENTER, 0, 52);
    lv_obj_set_style_bg_color(*bar, lv_color_hex(k_raised), 0);
    lv_obj_set_style_bg_color(*bar, lv_color_hex(k_accent), LV_PART_INDICATOR);
    return p;
}

// Shows the start screen and gives the user 2 s to press BOOT to force touch calibration.
static bool boot_pressed_after_start(lv_obj_t *scr)
{
    lv_obj_t *bar;
    lv_obj_t *splash = splash_create(scr, &bar);
    lv_refr_now(NULL);
    cyd_backlight(100);
    bool pressed = false;
    int shown = -1;
    for (uint32_t t0 = millis(); millis() - t0 < 2000 && !pressed;) {
        pressed = digitalRead(0) == LOW;
        const int v = (int)((millis() - t0) / 20);  // 0..100
        if (v / 5 != shown / 5) {  // redraw the bar in 5 % steps (only the bar area is sent)
            shown = v;
            lv_bar_set_value(bar, v > 100 ? 100 : v, LV_ANIM_OFF);
            lv_refr_now(NULL);
        }
        delay(20);
    }
    lv_obj_delete(splash);
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
    if (have) {
        cyd_set_rotation(load_rotation());  // the start screen already in the user's orientation
        if (boot_pressed_after_start(scr)) {
            have = false;
            cyd_set_rotation(0);  // calibration runs in landscape
        }
    }
    if (have) {
        cyd_touch_set_cal(&cal);
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
