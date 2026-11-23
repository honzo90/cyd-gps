/*
 * Resistive touch calibration: affine map raw (rx, ry) -> screen (x, y) from 3 taps.
 *   x = a*rx + b*ry + c,  y = d*rx + e*ry + f
 * Handles any rotation, mirroring and axis swap at once, so display orientation changes only
 * need a new calibration, not code changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float a, b, c, d, e, f;
} cyd_cal_t;

/* Solve from 3 screen targets and the raw readings taken there. 0 ok, -1 points degenerate
 * (collinear targets, or the same raw spot for different targets). */
int cyd_cal_solve(const int16_t sx[3], const int16_t sy[3], const uint16_t rx[3], const uint16_t ry[3],
                  cyd_cal_t *out);

/* Map and clamp into 0..w-1, 0..h-1. */
void cyd_cal_apply(const cyd_cal_t *cal, uint16_t rx, uint16_t ry, int16_t w, int16_t h, int16_t *x,
                   int16_t *y);

/* Community default for a landscape 320x240 CYD (raw x 200..3700 -> 0..319, raw y 240..3800 ->
 * 0..239, from witnessmenow/ESP32-Cheap-Yellow-Display LVGL9 example). Every unit differs a bit:
 * calibrate. Direction on the CYD2USB not verified. */
void cyd_cal_default(cyd_cal_t *out, int16_t w, int16_t h);

/* Sanity check for a stored calibration: finite, not degenerate, plausible scale
 * (a 12-bit ADC spanning a 240..320 px screen gives ~0.05-0.15 px per count). */
bool cyd_cal_valid(const cyd_cal_t *cal);

#ifdef __cplusplus
}
#endif
