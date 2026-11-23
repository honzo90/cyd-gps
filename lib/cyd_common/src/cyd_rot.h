/*
 * Screen rotation on the CYD (ST7789 240x320 panel, MADCTL) in 90 degree steps.
 *   rot 0 = landscape 320x240 (the orientation the touch calibration is made in)
 *   rot 1 = portrait 240x320, 2 = landscape upside down, 3 = portrait the other way
 * Consecutive values turn the picture by 90 degrees in the same direction. The touch keeps its
 * landscape calibration; cyd_rot_touch() turns the calibrated point to the rotated screen.
 * Verified for units with no extra mirror (CYD2USB here) or mirrored in both axes; a unit that
 * needs a mirror in one axis only may show the portrait orientations mirrored.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYD_ROT_LONG 320
#define CYD_ROT_SHORT 240

typedef struct {
    bool swap_xy, mirror_x, mirror_y;  // esp_lcd_panel_swap_xy / esp_lcd_panel_mirror
} cyd_rot_panel_t;

/* Panel settings for rot (0..3; other values = rot & 3). mx0/my0: the mirror this unit needs in
 * rot 0 (build flags CYD_LCD_MIRROR_X/Y). */
cyd_rot_panel_t cyd_rot_panel(uint8_t rot, bool mx0, bool my0);

/* Logical screen size for rot: 320 x 240 or 240 x 320. */
void cyd_rot_size(uint8_t rot, int16_t *w, int16_t *h);

/* Calibrated landscape touch point (0..319, 0..239) -> point on the rotated screen. */
void cyd_rot_touch(uint8_t rot, int16_t x, int16_t y, int16_t *u, int16_t *v);

#ifdef __cplusplus
}
#endif
