#include "xpt2046.h"

uint16_t xpt2046_convert(const xpt2046_bus_t *b, uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {
        b->mosi(b->ctx, (cmd >> i) & 1);
        b->delay_half(b->ctx);
        b->clk(b->ctx, 1);  /* XPT2046 samples DIN on the rising edge */
        b->delay_half(b->ctx);
        b->clk(b->ctx, 0);
    }
    b->mosi(b->ctx, 0);
    uint16_t w = 0;
    for (int i = 0; i < 16; i++) {
        b->delay_half(b->ctx);
        b->clk(b->ctx, 1);  /* DOUT changes on the falling edge: stable while high */
        w = (uint16_t)((w << 1) | (b->miso(b->ctx) ? 1 : 0));
        b->delay_half(b->ctx);
        b->clk(b->ctx, 0);
    }
    return (uint16_t)((w >> 3) & 0x0FFF);
}

static uint16_t dist(uint16_t a, uint16_t b)
{
    return a > b ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

uint16_t xpt2046_best_two_avg(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t ab = dist(a, b), ac = dist(a, c), bc = dist(b, c);
    if (ab <= ac && ab <= bc) {
        return (uint16_t)((a + b) / 2);
    }
    if (ac <= bc) {
        return (uint16_t)((a + c) / 2);
    }
    return (uint16_t)((b + c) / 2);
}

bool xpt2046_read(const xpt2046_bus_t *b, uint16_t z_threshold, xpt2046_raw_t *out)
{
    b->clk(b->ctx, 0);
    b->cs(b->ctx, 0);
    uint16_t z1 = xpt2046_convert(b, XPT2046_CMD_Z1);
    uint16_t z2 = xpt2046_convert(b, XPT2046_CMD_Z2);
    int z = (int)z1 + 4095 - (int)z2;
    bool touched = z >= (int)z_threshold;
    uint16_t x[3] = {0}, y[3] = {0};
    if (touched) {
        (void)xpt2046_convert(b, XPT2046_CMD_X);  /* first X after Z is noisy: discard */
        for (int i = 0; i < 3; i++) {
            x[i] = xpt2046_convert(b, XPT2046_CMD_X);
            y[i] = xpt2046_convert(b, i == 2 ? XPT2046_CMD_Y_PWRDN : XPT2046_CMD_Y);
        }
    } else {
        (void)xpt2046_convert(b, XPT2046_CMD_Y_PWRDN);
    }
    b->cs(b->ctx, 1);
    if (!touched) {
        return false;
    }
    out->x = xpt2046_best_two_avg(x[0], x[1], x[2]);
    out->y = xpt2046_best_two_avg(y[0], y[1], y[2]);
    out->z = (uint16_t)(z > 0xFFFF ? 0xFFFF : z);
    return true;
}
