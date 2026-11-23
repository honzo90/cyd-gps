// Host test for lib/gpx_core. Run: sh test/host/run.sh
#include <stdio.h>
#include <string.h>
#include "gpx_core.h"

static int s_fail, s_run;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++s_run;                                                        \
        if (!(cond)) {                                                  \
            ++s_fail;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

#define M_LAT (1.0 / 111195.0)  // 1 m north

static gpx_decision_t at(gpx_last_t *l, double north_m, uint32_t t_ms)
{
    return gpx_decide(l, 52.0 + north_m * M_LAT, 21.0, 1.0f, 8, t_ms);
}

static void test_first_point_logged(void)
{
    gpx_last_t l = {0};
    CHECK(at(&l, 0, 1000) == GPX_LOG);
    CHECK(l.have);
}

static void test_moving_every_5m_and_1s(void)
{
    gpx_last_t l = {0};
    at(&l, 0, 0);
    CHECK(at(&l, 3, 1000) == GPX_SKIP);    // only 3 m
    CHECK(at(&l, 6, 2000) == GPX_LOG);     // 6 m from the last logged point
    CHECK(at(&l, 20, 2500) == GPX_SKIP);   // far, but < 1 s since the last point
    CHECK(at(&l, 20, 3000) == GPX_LOG);
}

static void test_standing_logs_every_30s(void)
{
    gpx_last_t l = {0};
    at(&l, 0, 0);
    int logged = 0;
    for (uint32_t t = 1000; t <= 90000; t += 1000) {
        logged += at(&l, (t / 1000) % 2, t) != GPX_SKIP;  // 1 m jitter
    }
    CHECK(logged == 3);  // at 30, 60, 90 s
}

static void test_gap_starts_new_segment(void)
{
    gpx_last_t l = {0};
    at(&l, 0, 0);
    at(&l, 10, 1000);
    CHECK(at(&l, 500, 20000) == GPX_LOG_NEW_SEG);  // 19 s without a fix
    CHECK(at(&l, 510, 21000) == GPX_LOG);
}

static void test_bad_fixes_skipped_and_make_a_gap(void)
{
    gpx_last_t l = {0};
    at(&l, 0, 0);
    for (uint32_t t = 1000; t <= 8000; t += 1000) {
        CHECK(gpx_decide(&l, 52.0 + t * 0.01 * M_LAT, 21.0, 9.0f, 8, t) == GPX_SKIP);  // HDOP 9
    }
    CHECK(gpx_decide(&l, 52.0, 21.0, 1.0f, 3, 8500) == GPX_SKIP);  // 3 satellites
    CHECK(at(&l, 100, 9000) == GPX_LOG_NEW_SEG);
}

static void test_point_format(void)
{
    gpx_pt_t p = {52.2297, 21.0122, 112.4f, true, 2026, 11, 23, 11, 46, 2, 9, 1.25f};
    char buf[256];
    gpx_point(buf, sizeof buf, &p);
    CHECK(strcmp(buf, "   <trkpt lat=\"52.2297000\" lon=\"21.0122000\"><ele>112.4</ele>"
                      "<time>2026-11-23T11:46:02Z</time><sat>9</sat><hdop>1.2</hdop></trkpt>\n") == 0 ||
          strcmp(buf, "   <trkpt lat=\"52.2297000\" lon=\"21.0122000\"><ele>112.4</ele>"
                      "<time>2026-11-23T11:46:02Z</time><sat>9</sat><hdop>1.3</hdop></trkpt>\n") == 0);
    p.has_ele = false;
    p.lat = -33.5;
    p.lon = -70.25;
    gpx_point(buf, sizeof buf, &p);
    CHECK(strstr(buf, "lat=\"-33.5000000\" lon=\"-70.2500000\"><time>") != NULL);
    CHECK(strstr(buf, "<ele>") == NULL);
}

static void test_filename_and_document(void)
{
    gpx_pt_t p = {0, 0, 0, false, 2026, 1, 5, 7, 8, 9, 0, 0};
    char name[40];
    gpx_filename(name, sizeof name, &p);
    CHECK(strcmp(name, "/gpx/20260105-070809.gpx") == 0);

    char doc[1024];
    int n = gpx_header(doc, sizeof doc, "20260105-070809");
    n += gpx_point(doc + n, sizeof doc - n, &p);
    n += snprintf(doc + n, sizeof doc - n, "%s", GPX_NEW_SEG);
    n += gpx_point(doc + n, sizeof doc - n, &p);
    snprintf(doc + n, sizeof doc - n, "%s", GPX_FOOTER);
    // balanced tags
    int open_seg = 0, close_seg = 0;
    for (const char *q = doc; (q = strstr(q, "<trkseg>")); ++q) open_seg++;
    for (const char *q = doc; (q = strstr(q, "</trkseg>")); ++q) close_seg++;
    CHECK(open_seg == 2 && close_seg == 2);
    CHECK(strstr(doc, "<gpx version=\"1.1\"") != NULL);
    CHECK(strstr(doc, "</trk>\n</gpx>\n") != NULL);
}

int main(void)
{
    test_first_point_logged();
    test_moving_every_5m_and_1s();
    test_standing_logs_every_30s();
    test_gap_starts_new_segment();
    test_bad_fixes_skipped_and_make_a_gap();
    test_point_format();
    test_filename_and_document();
    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
