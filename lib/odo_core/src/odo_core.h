// Odometer logic, no Arduino/LVGL: host-testable (test/host/test_odo.c).
// Feed one GPS fix at a time; distance is added only for good fixes while moving.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Quality / plausibility limits.
#define ODO_MAX_HDOP 4.0f        // worse fixes are ignored and break the track
#define ODO_MIN_SATS 4
#define ODO_MIN_KMH 3.0f         // below this the receiver is treated as standing (jitter, not travel)
#define ODO_MIN_DT_MS 500        // GGA and RMC both update the position; ignore the 2nd one of a second
#define ODO_MAX_GAP_MS 5000      // longer gap: re-anchor, do not draw a straight line across it
#define ODO_MAX_MPS 70.0         // 252 km/h: a longer step is a position jump, not travel
#define ODO_MAX_HDOP_FOR_MAX 2.5f  // max speed only from good fixes (spikes come from weak ones)
#define ODO_MIN_SATS_FOR_MAX 5
#define ODO_MAX_KMH 300.0f

typedef struct {
    double lat, lon;   // degrees
    float speed_kmh;   // receiver's own speed (Doppler), not derived from positions
    float hdop;        // 0 = unknown
    uint8_t sats;      // used in fix
    uint32_t t_ms;     // when the fix arrived (millis)
} odo_fix_t;

typedef struct {
    // Persistent values.
    double trip_m;
    double total_m;
    float max_kmh;       // within the trip
    uint32_t moving_ms;  // within the trip
    // Track anchor (not persisted).
    bool have_prev;
    double prev_lat, prev_lon;
    uint32_t prev_ms;
} odo_state_t;

double odo_dist_m(double lat1, double lon1, double lat2, double lon2);  // haversine

// Returns true when the values to persist changed.
bool odo_step(odo_state_t *s, const odo_fix_t *f);
void odo_lost(odo_state_t *s);        // no fix: next good fix starts a new segment
void odo_reset_trip(odo_state_t *s);  // trip, max and moving time; total stays

#ifdef __cplusplus
}
#endif
