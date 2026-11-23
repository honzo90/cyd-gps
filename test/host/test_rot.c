// Host test for lib/cyd_common/src/cyd_rot.c. Run: sh test/host/run.sh
// Model of the ST7789: a logical pixel (x, y) lands in frame memory at (col 0..239, row 0..319):
//   MV=0: col = x, row = y;  MV=1: col = y, row = x;  then MX reverses col, MY reverses row.
// Whether MX/MY act before or after the MV exchange is not clear from the datasheet. For this unit
// (no extra mirror, CYD_LCD_MIRROR_X=Y=0) and for units mirrored in both axes both readings give
// the same result, checked below. A unit mirrored in one axis only would get mirrored portraits
// under the "before" reading: not supported, documented in cyd_rot.h.
#include <stdio.h>
#include "cyd_rot.h"

static int s_fail, s_run;
#define CHECK(cond)                                                \
    do {                                                           \
        ++s_run;                                                   \
        if (!(cond)) {                                             \
            ++s_fail;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

// after = 1: MX/MY applied to the memory address after the exchange; 0: before (to x / y)
static void phys(cyd_rot_panel_t p, int after, int x, int y, int *col, int *row)
{
    if (!p.swap_xy) {
        *col = p.mirror_x ? 239 - x : x;
        *row = p.mirror_y ? 319 - y : y;
    } else if (after) {
        *col = p.mirror_x ? 239 - y : y;
        *row = p.mirror_y ? 319 - x : x;
    } else {
        *row = p.mirror_x ? 319 - x : x;
        *col = p.mirror_y ? 239 - y : y;
    }
}

// Every rotation shows the same picture turned, never mirrored: a touch at a physical spot must
// map to the logical pixel that is drawn at that spot.
static void test_touch_matches_picture(void)
{
    for (int model = 0; model < 2; ++model) {
        for (int m = 0; m < 4; ++m) {
            const bool mx0 = m & 1, my0 = m & 2;
            const cyd_rot_panel_t base = cyd_rot_panel(0, mx0, my0);
            for (uint8_t rot = 0; rot < 4; ++rot) {
                const cyd_rot_panel_t p = cyd_rot_panel(rot, mx0, my0);
                int16_t w, h;
                cyd_rot_size(rot, &w, &h);
                int bad = 0;
                for (int x = 0; x < 320; x += 7) {
                    for (int y = 0; y < 240; y += 7) {
                        int c0, r0, c1, r1;
                        int16_t u, v;
                        phys(base, model, x, y, &c0, &r0);  // where landscape (x, y) is on the glass
                        cyd_rot_touch(rot, (int16_t)x, (int16_t)y, &u, &v);
                        bad += u < 0 || v < 0 || u >= w || v >= h;
                        phys(p, model, u, v, &c1, &r1);  // where the rotated UI draws (u, v)
                        bad += c0 != c1 || r0 != r1;
                    }
                }
                if (model == 0 && mx0 != my0) {
                    continue;  // see the note at the top
                }
                if (bad) {
                    printf("model %d mx0 %d my0 %d rot %d: %d bad\n", model, mx0, my0, rot, bad);
                }
                CHECK(bad == 0);
            }
        }
    }
}

static void test_sizes_and_steps(void)
{
    int16_t w, h;
    cyd_rot_size(0, &w, &h);
    CHECK(w == 320 && h == 240);
    cyd_rot_size(1, &w, &h);
    CHECK(w == 240 && h == 320);
    cyd_rot_size(2, &w, &h);
    CHECK(w == 320 && h == 240);
    cyd_rot_size(3, &w, &h);
    CHECK(w == 240 && h == 320);
    cyd_rot_size(5, &w, &h);  // wraps
    CHECK(w == 240 && h == 320);
    // each step is the previous one turned by another 90 degrees the same way:
    // T(rot+1)(p) == R90(T(rot)(p)), R90 on a W x H frame: (x, y) -> (y, W-1-x)
    int bad = 0;
    for (uint8_t rot = 0; rot < 4; ++rot) {
        int16_t sw, sh;
        cyd_rot_size(rot, &sw, &sh);
        for (int x = 0; x < 320; x += 11) {
            for (int y = 0; y < 240; y += 11) {
                int16_t u, v, u2, v2;
                cyd_rot_touch(rot, (int16_t)x, (int16_t)y, &u, &v);
                cyd_rot_touch((uint8_t)(rot + 1), (int16_t)x, (int16_t)y, &u2, &v2);
                bad += u2 != v || v2 != sw - 1 - u;
            }
        }
    }
    CHECK(bad == 0);
    // corners: landscape top-left goes round the four corners
    int16_t u, v;
    cyd_rot_touch(0, 0, 0, &u, &v);
    CHECK(u == 0 && v == 0);
    cyd_rot_touch(2, 0, 0, &u, &v);
    CHECK(u == 319 && v == 239);
}

int main(void)
{
    test_touch_matches_picture();
    test_sizes_and_steps();
    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
