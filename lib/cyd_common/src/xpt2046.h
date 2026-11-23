/*
 * XPT2046 resistive touch controller over bit-banged SPI (mode 0, MSB first).
 * On the CYD its pins (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36) are not on a hardware SPI
 * bus that is free: bit-banging keeps VSPI (SPI3) for the SD card. XPT2046 allows <= 2.5 MHz.
 *
 * One conversion = 8 command clocks + 16 read clocks; result = 16-bit word >> 3 (12 bits).
 * Commands as in PaulStoffregen/XPT2046_Touchscreen v1.4 (whose raw "x"/"y" naming the
 * community calibration values for the CYD use): 0x91 "x", 0xD1 "y", 0xB1 Z1, 0xC1 Z2,
 * last command 0xD0 powers down with PENIRQ enabled. Pressure z = Z1 + 4095 - Z2.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XPT2046_CMD_X 0x91
#define XPT2046_CMD_Y 0xD1
#define XPT2046_CMD_Z1 0xB1
#define XPT2046_CMD_Z2 0xC1
#define XPT2046_CMD_Y_PWRDN 0xD0
#define XPT2046_Z_THRESHOLD 400  /* same as XPT2046_Touchscreen */

typedef struct {
    void (*cs)(void *ctx, int level);
    void (*clk)(void *ctx, int level);
    void (*mosi)(void *ctx, int level);
    int (*miso)(void *ctx);
    void (*delay_half)(void *ctx);  /* half clock period, >= 200 ns */
    void *ctx;
} xpt2046_bus_t;

typedef struct {
    uint16_t x;  /* 0..4095 */
    uint16_t y;
    uint16_t z;  /* pressure estimate, bigger = harder */
} xpt2046_raw_t;

/* One conversion; CS must already be low. */
uint16_t xpt2046_convert(const xpt2046_bus_t *bus, uint8_t cmd);

/* Average of the two closest of three samples (rejects one outlier). */
uint16_t xpt2046_best_two_avg(uint16_t a, uint16_t b, uint16_t c);

/* Full read: pressure first; when z >= z_threshold, 3 x/y pairs filtered. Returns true when
 * touched (out filled), false when not touched. Always ends with the power-down command. */
bool xpt2046_read(const xpt2046_bus_t *bus, uint16_t z_threshold, xpt2046_raw_t *out);

#ifdef __cplusplus
}
#endif
