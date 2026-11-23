#include "odometer.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// Flash wear: NVS spreads writes, but still only write when something changed and not too often.
// At most one save per 100 m of trip or per 60 s; a power cut loses at most that.
static const double k_save_every_m = 100.0;
static const uint32_t k_save_every_ms = 60000;

static odo_state_t s_odo;
static double s_saved_trip_m;
static bool s_dirty;
static uint32_t s_last_save_ms;

static void save()
{
#ifdef GPS_SIM
    // Simulated kilometres must never end up in the real odometer.
    s_saved_trip_m = s_odo.trip_m;
    s_dirty = false;
    s_last_save_ms = millis();
    return;
#endif
    Preferences p;
    if (p.begin("odo", false)) {
        p.putDouble("trip", s_odo.trip_m);
        p.putDouble("total", s_odo.total_m);
        p.putFloat("max", s_odo.max_kmh);
        p.putUInt("mov", s_odo.moving_ms);
        p.end();
    }
    s_saved_trip_m = s_odo.trip_m;
    s_dirty = false;
    s_last_save_ms = millis();
}

void odometer_begin()
{
    memset(&s_odo, 0, sizeof s_odo);
    Preferences p;
    if (p.begin("odo", true)) {  // read-only: fails (and leaves zeros) on the very first boot
        s_odo.trip_m = p.getDouble("trip", 0);
        s_odo.total_m = p.getDouble("total", 0);
        s_odo.max_kmh = p.getFloat("max", 0);
        s_odo.moving_ms = p.getUInt("mov", 0);
        p.end();
    }
    s_saved_trip_m = s_odo.trip_m;
    s_last_save_ms = millis();
    Serial.printf("ODO: trip %.3f km, total %.3f km\n", s_odo.trip_m / 1000, s_odo.total_m / 1000);
}

void odometer_feed(const gps_data_t *g)
{
    if (!g->fix) {
        odo_lost(&s_odo);
        return;
    }
    const odo_fix_t f = {g->lat, g->lon, (float)g->speed_kmh, (float)g->hdop, (uint8_t)g->sats, g->fix_ms};
    s_dirty |= odo_step(&s_odo, &f);
    if (s_dirty && (s_odo.trip_m - s_saved_trip_m >= k_save_every_m || millis() - s_last_save_ms >= k_save_every_ms)) {
        save();
    }
}

void odometer_reset_trip()
{
    odo_reset_trip(&s_odo);
    save();
    Serial.println("ODO: trip reset");
}

void odometer_save()
{
    if (s_dirty) {
        save();
    }
}

void odometer_reset_all()
{
    odo_reset_trip(&s_odo);
    s_odo.total_m = 0;
    save();
    Serial.println("ODO: all reset");
}

const odo_state_t *odometer_get()
{
    return &s_odo;
}
