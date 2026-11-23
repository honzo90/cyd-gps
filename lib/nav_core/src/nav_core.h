// Direction of travel from the GPS course over ground (no compass: valid only while moving).
// No Arduino: host-tested (test/host/test_nav.c).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAV_SHOW_KMH 5.0f  // course shown from this speed (below it the GPS course is noise)
#define NAV_HIDE_KMH 3.0f  // ... and hidden again below this (hysteresis: no flicker at 4 km/h)

// "N", "NE", "E", "SE", "S", "SW", "W", "NW" for a course in degrees (any value, wraps).
const char *nav_cardinal(float deg);

// Course in 0..359 whole degrees (wraps, rounds: 359.6 -> 0).
int nav_deg(float deg);

// Whether to show the course now; *shown keeps the state between calls.
bool nav_show(bool *shown, bool fix, bool course_valid, float kmh);

#ifdef __cplusplus
}
#endif
