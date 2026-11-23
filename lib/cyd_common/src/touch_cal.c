#include "touch_cal.h"
#include <math.h>

int cyd_cal_solve(const int16_t sx[3], const int16_t sy[3], const uint16_t rx[3], const uint16_t ry[3],
                  cyd_cal_t *out)
{
    /* Cramer's rule on | rx ry 1 | rows */
    double r0x = rx[0], r0y = ry[0], r1x = rx[1], r1y = ry[1], r2x = rx[2], r2y = ry[2];
    double det = r0x * (r1y - r2y) - r0y * (r1x - r2x) + (r1x * r2y - r2x * r1y);
    /* raw triangle area must be meaningful: < ~100x100 counts means the taps were not spread */
    if (fabs(det) < 2.0 * 100.0 * 100.0) {
        return -1;
    }
    double tdet = (double)sx[0] * (sy[1] - sy[2]) - (double)sy[0] * (sx[1] - sx[2]) +
                  ((double)sx[1] * sy[2] - (double)sx[2] * sy[1]);
    if (fabs(tdet) < 1.0) {
        return -1;  /* collinear screen targets */
    }
    const int16_t *s[2] = {sx, sy};
    double coef[2][3];
    for (int k = 0; k < 2; k++) {
        double v0 = s[k][0], v1 = s[k][1], v2 = s[k][2];
        coef[k][0] = (v0 * (r1y - r2y) - r0y * (v1 - v2) + (v1 * r2y - v2 * r1y)) / det;
        coef[k][1] = (r0x * (v1 - v2) - v0 * (r1x - r2x) + (r1x * v2 - r2x * v1)) / det;
        coef[k][2] = (r0x * (r1y * v2 - r2y * v1) - r0y * (r1x * v2 - r2x * v1) + v0 * (r1x * r2y - r2x * r1y)) / det;
    }
    out->a = (float)coef[0][0];
    out->b = (float)coef[0][1];
    out->c = (float)coef[0][2];
    out->d = (float)coef[1][0];
    out->e = (float)coef[1][1];
    out->f = (float)coef[1][2];
    return 0;
}

static int16_t clampi(float v, int16_t n)
{
    if (!(v >= 0.0f)) {  /* also catches NaN */
        return 0;
    }
    if (v > (float)(n - 1)) {
        return (int16_t)(n - 1);
    }
    return (int16_t)(v + 0.5f);
}

void cyd_cal_apply(const cyd_cal_t *k, uint16_t rx, uint16_t ry, int16_t w, int16_t h, int16_t *x, int16_t *y)
{
    *x = clampi(k->a * rx + k->b * ry + k->c, w);
    *y = clampi(k->d * rx + k->e * ry + k->f, h);
}

void cyd_cal_default(cyd_cal_t *out, int16_t w, int16_t h)
{
    out->a = (float)w / (3700.0f - 200.0f);
    out->b = 0.0f;
    out->c = -200.0f * out->a;
    out->d = 0.0f;
    out->e = (float)h / (3800.0f - 240.0f);
    out->f = -240.0f * out->e;
}

bool cyd_cal_valid(const cyd_cal_t *k)
{
    const float v[6] = {k->a, k->b, k->c, k->d, k->e, k->f};
    for (int i = 0; i < 6; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    float sx = sqrtf(k->a * k->a + k->b * k->b);  /* px per raw count along screen x */
    float sy = sqrtf(k->d * k->d + k->e * k->e);
    float det = k->a * k->e - k->b * k->d;
    return sx > 0.02f && sx < 0.5f && sy > 0.02f && sy < 0.5f && fabsf(det) > 0.0004f;
}
