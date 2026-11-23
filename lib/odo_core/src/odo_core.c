#include "odo_core.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double odo_dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0, k = M_PI / 180.0;
    const double dp = (lat2 - lat1) * k, dl = (lon2 - lon1) * k;
    double a = sin(dp / 2) * sin(dp / 2) + cos(lat1 * k) * cos(lat2 * k) * sin(dl / 2) * sin(dl / 2);
    if (a > 1) {
        a = 1;
    }
    return 2 * r * asin(sqrt(a));
}

static void anchor(odo_state_t *s, const odo_fix_t *f)
{
    s->have_prev = true;
    s->prev_lat = f->lat;
    s->prev_lon = f->lon;
    s->prev_ms = f->t_ms;
}

bool odo_step(odo_state_t *s, const odo_fix_t *f)
{
    if (f->hdop <= 0 || f->hdop > ODO_MAX_HDOP || f->sats < ODO_MIN_SATS) {
        odo_lost(s);
        return false;
    }
    bool changed = false;
    if (f->speed_kmh > s->max_kmh && f->speed_kmh <= ODO_MAX_KMH && f->hdop <= ODO_MAX_HDOP_FOR_MAX &&
        f->sats >= ODO_MIN_SATS_FOR_MAX) {
        s->max_kmh = f->speed_kmh;
        changed = true;
    }
    if (!s->have_prev) {
        anchor(s, f);
        return changed;
    }
    const uint32_t dt = f->t_ms - s->prev_ms;  // unsigned: wraps correctly
    if (dt < ODO_MIN_DT_MS) {
        return changed;  // same second, keep the anchor
    }
    if (dt > ODO_MAX_GAP_MS) {
        anchor(s, f);
        return changed;
    }
    const double d = odo_dist_m(s->prev_lat, s->prev_lon, f->lat, f->lon);
    if (d > ODO_MAX_MPS * dt / 1000.0 + 5.0) {
        anchor(s, f);  // jump: start again from here
        return changed;
    }
    if (f->speed_kmh >= ODO_MIN_KMH) {
        s->trip_m += d;
        s->total_m += d;
        s->moving_ms += dt;
        changed = true;
    }
    anchor(s, f);  // also while standing, so drift does not pile up for the next move
    return changed;
}

void odo_lost(odo_state_t *s)
{
    s->have_prev = false;
}

void odo_reset_trip(odo_state_t *s)
{
    s->trip_m = 0;
    s->max_kmh = 0;
    s->moving_ms = 0;
    s->have_prev = false;
}
