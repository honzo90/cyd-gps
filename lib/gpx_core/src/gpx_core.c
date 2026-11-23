#include "gpx_core.h"
#include <stdio.h>
#include "odo_core.h"  // odo_dist_m

const char GPX_FOOTER[] = "  </trkseg>\n </trk>\n</gpx>\n";
const char GPX_NEW_SEG[] = "  </trkseg>\n  <trkseg>\n";

gpx_decision_t gpx_decide(gpx_last_t *last, double lat, double lon, float hdop, uint8_t sats, uint32_t t_ms)
{
    if (hdop <= 0 || hdop > GPX_MAX_HDOP || sats < GPX_MIN_SATS) {
        return GPX_SKIP;  // a bad fix does not count as "seen", so a long bad stretch opens a new segment
    }
    const bool gap = last->have && t_ms - last->seen_ms > GPX_SEG_GAP_MS;
    last->seen_ms = t_ms;
    if (last->have && !gap) {
        const uint32_t dt = t_ms - last->t_ms;
        if (dt < GPX_MIN_DT_MS) {
            return GPX_SKIP;
        }
        if (odo_dist_m(last->lat, last->lon, lat, lon) < GPX_MIN_MOVE_M && dt < GPX_MAX_IDLE_MS) {
            return GPX_SKIP;
        }
    }
    const gpx_decision_t d = gap ? GPX_LOG_NEW_SEG : GPX_LOG;
    last->have = true;
    last->lat = lat;
    last->lon = lon;
    last->t_ms = t_ms;
    return d;
}

int gpx_filename(char *buf, size_t n, const gpx_pt_t *p)
{
    return snprintf(buf, n, "/gpx/%04u%02u%02u-%02u%02u%02u.gpx", p->year, p->month, p->day, p->hh, p->mm, p->ss);
}

int gpx_header(char *buf, size_t n, const char *track_name)
{
    return snprintf(buf, n,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<gpx version=\"1.1\" creator=\"CYD GPS\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
                    " <trk>\n"
                    "  <name>%s</name>\n"
                    "  <trkseg>\n",
                    track_name);
}

int gpx_point(char *buf, size_t n, const gpx_pt_t *p)
{
    // GPX 1.1 order inside <trkpt>: ele, time, ..., sat, hdop
    char ele[32] = "";
    if (p->has_ele) {
        snprintf(ele, sizeof ele, "<ele>%.1f</ele>", p->ele_m);
    }
    return snprintf(buf, n,
                    "   <trkpt lat=\"%.7f\" lon=\"%.7f\">%s<time>%04u-%02u-%02uT%02u:%02u:%02uZ</time>"
                    "<sat>%u</sat><hdop>%.1f</hdop></trkpt>\n",
                    p->lat, p->lon, ele, p->year, p->month, p->day, p->hh, p->mm, p->ss, p->sats, p->hdop);
}
