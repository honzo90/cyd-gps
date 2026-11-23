// Host test for lib/nav_core. Run: sh test/host/run.sh
#include <stdio.h>
#include <string.h>
#include "nav_core.h"

static int s_fail, s_run;
#define CHECK(cond)                                                \
    do {                                                           \
        ++s_run;                                                   \
        if (!(cond)) {                                             \
            ++s_fail;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)
#define IS(deg, name) CHECK(strcmp(nav_cardinal(deg), name) == 0)

int main(void)
{
    IS(0, "N");
    IS(22.4f, "N");
    IS(22.6f, "NE");
    IS(45, "NE");
    IS(90, "E");
    IS(135, "SE");
    IS(180, "S");
    IS(225, "SW");
    IS(270, "W");
    IS(315, "NW");
    IS(337.4f, "NW");
    IS(337.6f, "N");
    IS(359.9f, "N");
    IS(360, "N");
    IS(-90, "W");
    IS(450, "E");

    CHECK(nav_deg(0) == 0);
    CHECK(nav_deg(359.6f) == 0);
    CHECK(nav_deg(359.4f) == 359);
    CHECK(nav_deg(-1) == 359);
    CHECK(nav_deg(725) == 5);

    bool shown = false;
    CHECK(!nav_show(&shown, true, true, 4));   // not yet
    CHECK(nav_show(&shown, true, true, 5));    // moving
    CHECK(nav_show(&shown, true, true, 3.5f)); // hysteresis: still shown
    CHECK(!nav_show(&shown, true, true, 2.9f));
    CHECK(!nav_show(&shown, true, true, 4));   // must reach 5 again
    CHECK(nav_show(&shown, true, true, 50));
    CHECK(!nav_show(&shown, false, true, 50)); // no fix
    CHECK(nav_show(&shown, true, true, 50));
    CHECK(!nav_show(&shown, true, false, 50)); // course not valid

    printf("%d/%d checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
