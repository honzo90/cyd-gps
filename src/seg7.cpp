#include "seg7.h"

static const uint32_t k_unlit = 0x1A2026;  // faint "off" segments, like a real LCD

// Segment bits: a=0 (top), b=1 (top right), c=2 (bottom right), d=3 (bottom), e=4 (bottom left),
// f=5 (top left), g=6 (middle).
static const uint8_t k_digit[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
static const uint8_t k_dash = 0x40;

void seg7_create(seg7_t *s, lv_obj_t *parent, int x, int y, int w, int h, int t, int sp)
{
    const int g = 2;                       // small gap where segments meet
    const int hl = w - t - 2 * g;          // horizontal segment length
    const int vl = h / 2 - t / 2 - 2 * g;  // vertical segment length
    for (int d = 0; d < SEG7_DIGITS; ++d) {
        const int ox = x + d * (w + sp);
        const int r[7][4] = {
            {ox + t / 2 + g, y, hl, t},                          // a
            {ox + w - t, y + t / 2 + g, t, vl},                  // b
            {ox + w - t, y + h / 2 + g, t, vl},                  // c
            {ox + t / 2 + g, y + h - t, hl, t},                  // d
            {ox, y + h / 2 + g, t, vl},                          // e
            {ox, y + t / 2 + g, t, vl},                          // f
            {ox + t / 2 + g, y + h / 2 - t / 2, hl, t},          // g
        };
        for (int i = 0; i < 7; ++i) {
            lv_obj_t *o = lv_obj_create(parent);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, r[i][0], r[i][1]);
            lv_obj_set_size(o, r[i][2], r[i][3]);
            lv_obj_set_style_radius(o, t / 2, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(k_unlit), 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // swipes go to the tileview
            s->seg[d][i] = o;
        }
        s->shown[d] = 0;
    }
    s->color = 0;
}

void seg7_set(seg7_t *s, int value, uint32_t color)
{
    uint8_t mask[SEG7_DIGITS];
    if (value < 0) {
        for (uint8_t &m : mask) {
            m = k_dash;
        }
    } else {
        if (value > 999) {
            value = 999;
        }
        for (int d = SEG7_DIGITS - 1; d >= 0; --d) {
            const bool blank = value == 0 && d < SEG7_DIGITS - 1;  // no leading zeros
            mask[d] = blank ? 0 : k_digit[value % 10];
            value /= 10;
        }
    }
    const bool recolor = color != s->color;
    for (int d = 0; d < SEG7_DIGITS; ++d) {
        if (mask[d] == s->shown[d] && !recolor) {
            continue;
        }
        for (int i = 0; i < 7; ++i) {
            const bool on = mask[d] & (1u << i);
            lv_obj_set_style_bg_color(s->seg[d][i], lv_color_hex(on ? color : k_unlit), 0);
        }
        s->shown[d] = mask[d];
    }
    s->color = color;
}
