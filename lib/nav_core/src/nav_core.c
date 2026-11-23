#include "nav_core.h"
#include <math.h>

static float wrap(float deg)
{
    float d = fmodf(deg, 360.0f);
    return d < 0 ? d + 360.0f : d;
}

const char *nav_cardinal(float deg)
{
    static const char *const k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return k[(int)((wrap(deg) + 22.5f) / 45.0f) % 8];
}

int nav_deg(float deg)
{
    return (int)lroundf(wrap(deg)) % 360;
}

bool nav_show(bool *shown, bool fix, bool course_valid, float kmh)
{
    if (!fix || !course_valid) {
        *shown = false;
    } else if (kmh >= NAV_SHOW_KMH) {
        *shown = true;
    } else if (kmh < NAV_HIDE_KMH) {
        *shown = false;
    }
    return *shown;
}
