// Cheap Yellow Display (CYD2USB) on Arduino-ESP32 2.0.x + LVGL 9. LVGL only from loop().
#pragma once
#include <lvgl.h>
#include "touch_cal.h"

#define CYD_H_RES 320  // landscape (MADCTL MX|MV = TFT_eSPI setRotation(1))
#define CYD_V_RES 240

bool cyd_begin(uint32_t spi_hz = 40000000);
void cyd_loop();
void cyd_backlight(uint8_t percent);
void cyd_led(bool r, bool g, bool b);
bool cyd_touch_raw(uint16_t *rx, uint16_t *ry);
void cyd_touch_set_cal(const cyd_cal_t *cal);
bool cyd_cal_load(cyd_cal_t *cal);  // Preferences "cyd"/"cal"; false when missing/invalid
bool cyd_cal_save(const cyd_cal_t *cal);
