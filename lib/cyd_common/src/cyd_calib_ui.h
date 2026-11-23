/*
 * 3-point touch calibration screen in LVGL 9. Shows a target, waits for a steady press,
 * averages raw samples, waits for release, then the next target. Runs inside the LVGL
 * context (timer), so call it with the LVGL lock held / from the LVGL thread.
 * The overlay sits on lv_layer_top() and swallows clicks, so the UI below is not touched.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "touch_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw reading without calibration; true while pressed. Called from the LVGL timer. */
typedef bool (*cyd_raw_read_fn)(uint16_t *rx, uint16_t *ry);
/* ok = false: the taps were unusable (cal untouched); the caller may restart. */
typedef void (*cyd_calib_done_fn)(const cyd_cal_t *cal, bool ok);

void cyd_calib_ui_start(lv_display_t *disp, cyd_raw_read_fn read_raw, cyd_calib_done_fn done);

#ifdef __cplusplus
}
#endif
