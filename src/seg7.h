// Large seven-segment digits drawn with plain LVGL objects (no font needed, any size).
#pragma once
#include <lvgl.h>

#define SEG7_DIGITS 3

struct seg7_t {
    lv_obj_t *seg[SEG7_DIGITS][7];  // a b c d e f g
    uint8_t shown[SEG7_DIGITS];      // current segment masks (avoid needless redraws)
    uint32_t color;
};

// Digit cell w x h, stroke t, gap between digits sp; top-left at (x, y).
void seg7_create(seg7_t *s, lv_obj_t *parent, int x, int y, int w, int h, int t, int sp);
// 0..999, right-aligned without leading zeros; value < 0 shows "---".
void seg7_set(seg7_t *s, int value, uint32_t color);
