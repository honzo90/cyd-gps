// Renders every CYD screen to PNG on a Mac, from the real src/main.cpp and LVGL 9.2 (same pixels as
// the device), with made-up data. Run: sh tools/screens/render.sh [outdir]
// Nothing here is real: synthetic position, network name and password.
#include "Arduino.h"
#include <math.h>
#include <stdlib.h>

#define setup app_setup
#define loop app_loop
#include "../../src/main.cpp"
#undef setup
#undef loop

HostSerial Serial;
HostEsp ESP;
static uint32_t s_host_ms = 100000;
uint32_t millis() { return s_host_ms; }

// ---- scenario -------------------------------------------------------------------------------
static int s_scene;  // 0 driving, 1 standing ready / no runs

void gps_begin() {}
void gps_poll() {}
uint32_t gps_fix_seq() { return 1; }
uint32_t gps_spd_seq() { return 1; }
void gps_toggle_echo() {}
void gps_get(gps_data_t *g)
{
    memset(g, 0, sizeof *g);
    g->fix = true;
    g->lat = 50.061234;  // made up
    g->lon = 19.937654;
    g->alt_m = 231.4;
    g->alt_valid = true;
    g->speed_kmh = g->spd_kmh = s_scene == 0 ? 87.3f : 0;
    g->course_deg = 62;
    g->course_valid = s_scene == 0;
    g->sats = 11;
    g->hdop = 0.9;
    g->time_valid = g->date_valid = true;
    g->hh = 15; g->mm = 42; g->ss = 7; g->year = 2026; g->month = 11; g->day = 24;
    g->chars = 1234567;
    g->baud = 115200;
    g->baud_locked = true;
    g->sat_view = 18; g->sat_tracked = 14; g->sat_strong = 9; g->snr_max = 44; g->snr_top4 = 41;
    g->spd_valid = true;
    g->rate_hz = 10;
}

void accel_begin() {}
void accel_feed(const gps_data_t *) {}
void accel_reset_best() {}
void accel_restart() {}
void accel_clear_history() {}
void accel_get(accel_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->rate_hz = 10;
    v->sample_ok = true;
    for (int i = 0; i < ACC_N; ++i) {
        v->last.t[i] = v->best[i] = -1;
        v->best_grade[i] = NAN;
    }
    v->phase = s_scene == 0 ? ACC_IDLE : ACC_ARMED;
    if (s_scene == 0) {
        const float last[ACC_N] = {3.12f, 7.85f, -1, -1, 5.87f, 15.41f};  // 100 m = a new record
        const float best[ACC_N] = {2.98f, 7.41f, -1, -1, 5.87f, 15.10f};
        for (int i = 0; i < ACC_N; ++i) {
            v->last.t[i] = last[i];
            v->best[i] = best[i];
        }
        v->has_last = true;
        v->elapsed_s = 15.41f;
        v->last.v_max = 152;
        v->last.grade_valid = true;
        v->last.grade_pct = -1.2f;
        v->stats.runs = 6;
        v->stats.n[ACC_T_0_100] = 5;
        v->stats.sum[ACC_T_0_100] = 5 * 7.7f;
        v->stats.sq[ACC_T_0_100] = 5 * 7.7f * 7.7f + 0.2f;
    }
}
int accel_recent(acc_hist_row_t *rows, int max)
{
    const char *ids[] = {"20261124-154000", "20261124-153100", "20261124-152200", "R000003", "20261124-150500",
                         "20261124-145000", "20261124-144000", "20261124-143000"};
    int n = s_scene == 0 ? (max < 8 ? max : 8) : 0;
    for (int i = 0; i < n; ++i) {
        memset(&rows[i], 0, sizeof rows[i]);
        snprintf(rows[i].id, sizeof rows[i].id, "%s", ids[i]);
        for (int k = 0; k < ACC_N; ++k) rows[i].t[k] = -1;
        rows[i].t[ACC_T_0_100] = 7.4f + 0.13f * i;
        rows[i].t[ACC_T_QUARTER] = i == 3 ? -1 : 15.1f + 0.2f * i;
        rows[i].vmax = 150 - 3 * i;
        rows[i].grade_pct = i % 3 == 0 ? -1.2f : i % 3 == 1 ? 0.3f : 2.1f;
        rows[i].aborted = i == 3;
        rows[i].low_rate = i == 6;
    }
    return n;
}

void bootlog_begin() {}
void bootlog_tick() {}
void bootlog_print() {}

void gh_ota_check_now() {}
void gh_ota_install() {}
void gh_ota_get(gh_status_t *s) { memset(s, 0, sizeof *s); s->state = GH_UP_TO_DATE; snprintf(s->latest, sizeof s->latest, "1.0.17"); }
void gh_ota_text(char *b, size_t n) { snprintf(b, n, "Aktualna wersja (1.0.17)"); }

void gpx_log_begin() {}
void gpx_log_feed(const gps_data_t *) {}
void gpx_log_poll() {}
void gpx_log_new_track() {}
void gpx_log_status(char *b, size_t n) { snprintf(b, n, "SD: 20261124-151742.gpx, 1834 pkt"); }
uint32_t gpx_log_current(char *name, size_t n) { snprintf(name, n, "20261124-151742.gpx"); return 1834; }
void gpx_log_print_files() {}
const char *gpx_log_delete(const char *) { return "ok"; }
bool gpx_log_sd_ok() { return true; }

void net_begin() {}
void net_poll() {}
void net_get(net_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = true;
    s->sta_configured = s->sta_connected = true;
    s->rssi = -58;
    snprintf(s->sta_ssid, sizeof s->sta_ssid, "PrzykladowaSiec");
    snprintf(s->sta_ip, sizeof s->sta_ip, "192.168.1.50");
    snprintf(s->dev_pass, sizeof s->dev_pass, "abcd-1234");
    s->ntp_ok = true;
}
void net_set_enabled(bool) {}
bool net_ntp_time(char *b, size_t n) { snprintf(b, n, "15:42:07"); return true; }
const char *net_user() { return "cyd"; }

static odo_state_t s_odo;
void odometer_begin() {}
void odometer_feed(const gps_data_t *) {}
void odometer_reset_trip() {}
void odometer_reset_all() {}
const odo_state_t *odometer_get()
{
    s_odo.trip_m = 23480;
    s_odo.total_m = 1287400;
    s_odo.max_kmh = 152.4f;
    s_odo.moving_ms = 27 * 60000 + 12000;
    return &s_odo;
}

void web_publish(const web_snapshot_t *) {}
bool web_login_url(char *b, size_t n) { snprintf(b, n, "http://192.168.1.50/login?c=EXAMPLE"); return true; }
void web_login_cancel() {}
void web_logout_all() {}
int web_take_rotation() { return -1; }
bool web_take_clear_runs() { return false; }

// ---- display --------------------------------------------------------------------------------
static lv_display_t *s_disp;
static uint16_t s_fb[320 * 320];
static uint8_t s_rot;

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const int w = lv_display_get_horizontal_resolution(d);
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x) s_fb[y * w + x] = *p++;
    lv_display_flush_ready(d);
}

bool cyd_begin(uint32_t) { return true; }
void cyd_loop() {}
void cyd_set_rotation(uint8_t r)
{
    s_rot = r & 3;
    lv_display_set_resolution(s_disp, s_rot & 1 ? 240 : 320, s_rot & 1 ? 320 : 240);
}
uint8_t cyd_rotation() { return s_rot; }
void cyd_backlight(uint8_t) {}
void cyd_led(bool, bool, bool) {}
bool cyd_touch_raw(uint16_t *, uint16_t *) { return false; }
void cyd_touch_set_cal(const cyd_cal_t *) {}
bool cyd_cal_load(cyd_cal_t *) { return true; }
bool cyd_cal_save(const cyd_cal_t *) { return true; }

static void save_png(const char *dir, const char *name)
{
    const int w = lv_display_get_horizontal_resolution(s_disp), h = lv_display_get_vertical_resolution(s_disp);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const uint16_t c = s_fb[i];
        const uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                                (uint8_t)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void settle()
{
    for (int i = 0; i < 40; ++i) {
        s_host_ms += 50;
        lv_timer_handler();
    }
    lv_refr_now(s_disp);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(millis);
    s_disp = lv_display_create(320, 240);
    static uint8_t buf[320 * 320 * 2];
    lv_display_set_buffers(s_disp, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(k_bg), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_ui();
    refresh(NULL);
    lv_timer_create(refresh, 500, NULL);
    static const char *names[5] = {"1-predkosc", "2-pomiar", "3-licznik", "4-gps", "5-wifi"};
    for (uint8_t rot = 0; rot < 2; ++rot) {
        if (rot) apply_rotation(1);
        for (s_scene = 0; s_scene < 2; ++s_scene) {
            for (int t = 0; t < 5; ++t) {
                if (s_scene == 1 && t != 1) continue;  // the 2nd scene only matters for POMIAR
                lv_tileview_set_tile_by_index(s_tv, t, 0, LV_ANIM_OFF);
                settle();
                char nm[64];
                snprintf(nm, sizeof nm, "%s-%s%s", rot ? "pion" : "poziom", names[t], s_scene ? "-gotowy" : "");
                save_png(dir, nm);
            }
        }
        s_scene = 0;
        lv_tileview_set_tile_by_index(s_tv, 1, 0, LV_ANIM_OFF);
        on_runs_open(NULL);
        settle();
        save_png(dir, rot ? "pion-2-pomiar-lista" : "poziom-2-pomiar-lista");
        runs_close();
        settle();
    }
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("LVGL heap after build: %lu B free (device budget %u B)\n", (unsigned long)m.free_size, LV_MEM_SIZE);
    return 0;
}
