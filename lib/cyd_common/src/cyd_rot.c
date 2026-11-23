#include "cyd_rot.h"

cyd_rot_panel_t cyd_rot_panel(uint8_t rot, bool mx0, bool my0)
{
    // rot 0 is MADCTL MV (+ this unit's mirror). Turning by 90 degrees steps through
    // MV -> MY -> MV|MX|MY -> MX (each relative to the unit's mirror), like TFT_eSPI's
    // setRotation 1 -> 2 -> 3 -> 0 on a panel mirrored in X.
    static const cyd_rot_panel_t k[4] = {
        {true, false, false},
        {false, false, true},
        {true, true, true},
        {false, true, false},
    };
    cyd_rot_panel_t p = k[rot & 3];
    p.mirror_x ^= mx0;
    p.mirror_y ^= my0;
    return p;
}

void cyd_rot_size(uint8_t rot, int16_t *w, int16_t *h)
{
    const bool portrait = rot & 1;
    *w = portrait ? CYD_ROT_SHORT : CYD_ROT_LONG;
    *h = portrait ? CYD_ROT_LONG : CYD_ROT_SHORT;
}

void cyd_rot_touch(uint8_t rot, int16_t x, int16_t y, int16_t *u, int16_t *v)
{
    switch (rot & 3) {
    case 0:
        *u = x;
        *v = y;
        break;
    case 1:
        *u = y;
        *v = CYD_ROT_LONG - 1 - x;
        break;
    case 2:
        *u = CYD_ROT_LONG - 1 - x;
        *v = CYD_ROT_SHORT - 1 - y;
        break;
    default:
        *u = CYD_ROT_SHORT - 1 - y;
        *v = x;
        break;
    }
}
