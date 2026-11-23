// GPX 1.1 track writing helpers, no Arduino/SD: host-testable (test/host/test_gpx.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPX_MIN_MOVE_M 5.0      // log a point after moving this far ...
#define GPX_MAX_IDLE_MS 30000   // ... or after this long without moving
#define GPX_MIN_DT_MS 1000      // never more often than this
#define GPX_SEG_GAP_MS 5000     // no fix for longer: start a new <trkseg>
#define GPX_MAX_HDOP 4.0f
#define GPX_MIN_SATS 4

typedef struct {
    double lat, lon;
    float ele_m;
    bool has_ele;
    uint16_t year;
    uint8_t month, day, hh, mm, ss;  // UTC
    uint8_t sats;
    float hdop;
} gpx_pt_t;

typedef struct {
    bool have;       // a point was logged in this file
    double lat, lon;
    uint32_t t_ms;   // when that point's fix arrived
    uint32_t seen_ms;  // last good fix seen (for gap detection)
} gpx_last_t;

typedef enum { GPX_SKIP = 0, GPX_LOG, GPX_LOG_NEW_SEG } gpx_decision_t;

// Decide what to do with a fix that arrived at t_ms. Updates *last when the point is to be logged.
gpx_decision_t gpx_decide(gpx_last_t *last, double lat, double lon, float hdop, uint8_t sats, uint32_t t_ms);

// "/gpx/20261123-114602.gpx"
int gpx_filename(char *buf, size_t n, const gpx_pt_t *p);
int gpx_header(char *buf, size_t n, const char *track_name);
int gpx_point(char *buf, size_t n, const gpx_pt_t *p);
extern const char GPX_FOOTER[];     // closes trkseg, trk and gpx
extern const char GPX_NEW_SEG[];    // closes the current trkseg and opens the next

#ifdef __cplusplus
}
#endif
