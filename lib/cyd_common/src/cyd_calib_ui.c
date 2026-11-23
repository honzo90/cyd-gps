#include "cyd_calib_ui.h"

#define SAMPLES 8       /* steady pressed samples averaged per target */
#define RELEASE_TICKS 3 /* released samples before the next target */
#define PERIOD_MS 20

static struct {
    lv_obj_t *overlay, *cross_h, *cross_v, *text;
    lv_timer_t *timer;
    cyd_raw_read_fn read;
    cyd_calib_done_fn done;
    int16_t sx[3], sy[3];
    uint16_t rx[3], ry[3];
    uint32_t sum_x, sum_y;
    int point, n, released;
    bool waiting_release;
} s;

static void place_target(void)
{
    lv_obj_set_pos(s.cross_h, s.sx[s.point] - 10, s.sy[s.point]);
    lv_obj_set_pos(s.cross_v, s.sx[s.point], s.sy[s.point] - 10);
    lv_label_set_text_fmt(s.text, "Touch calibration %d/3\nPress the centre of the cross\n(stylus or fingernail)",
                          s.point + 1);
}

static void finish(void)
{
    cyd_cal_t cal;
    bool ok = cyd_cal_solve(s.sx, s.sy, s.rx, s.ry, &cal) == 0 && cyd_cal_valid(&cal);
    lv_timer_delete(s.timer);
    lv_obj_delete(s.overlay);
    s.done(ok ? &cal : NULL, ok);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint16_t rx, ry;
    bool pressed = s.read(&rx, &ry);
    if (s.waiting_release) {
        s.released = pressed ? 0 : s.released + 1;
        if (s.released >= RELEASE_TICKS) {
            s.waiting_release = false;
            if (++s.point == 3) {
                finish();
                return;
            }
            place_target();
        }
        return;
    }
    if (!pressed) {
        s.n = 0;  /* a press must be continuous */
        s.sum_x = s.sum_y = 0;
        return;
    }
    s.sum_x += rx;
    s.sum_y += ry;
    if (++s.n == SAMPLES) {
        s.rx[s.point] = (uint16_t)(s.sum_x / SAMPLES);
        s.ry[s.point] = (uint16_t)(s.sum_y / SAMPLES);
        s.n = 0;
        s.sum_x = s.sum_y = 0;
        s.waiting_release = true;
        s.released = 0;
        lv_label_set_text(s.text, "OK - release");
    }
}

static lv_obj_t *bar(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xFF4040), 0);
    return o;
}

void cyd_calib_ui_start(lv_display_t *disp, cyd_raw_read_fn read_raw, cyd_calib_done_fn done)
{
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    s.read = read_raw;
    s.done = done;
    /* targets not in a line and away from the edges (resistive panels are worst there) */
    s.sx[0] = (int16_t)(w / 10);     s.sy[0] = (int16_t)(h / 10);
    s.sx[1] = (int16_t)(w * 9 / 10); s.sy[1] = (int16_t)(h / 2);
    s.sx[2] = (int16_t)(w / 2);      s.sy[2] = (int16_t)(h * 9 / 10);
    s.point = s.n = 0;
    s.sum_x = s.sum_y = 0;
    s.waiting_release = false;

    s.overlay = lv_obj_create(lv_display_get_layer_top(disp));
    lv_obj_remove_style_all(s.overlay);
    lv_obj_set_size(s.overlay, w, h);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.overlay, lv_color_black(), 0);
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_CLICKABLE);  /* swallow input */
    s.text = lv_label_create(s.overlay);
    lv_obj_set_style_text_color(s.text, lv_color_white(), 0);
    lv_obj_set_style_text_align(s.text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s.text);
    s.cross_h = bar(s.overlay, 21, 1);
    s.cross_v = bar(s.overlay, 1, 21);
    place_target();
    s.timer = lv_timer_create(tick, PERIOD_MS, NULL);
}
