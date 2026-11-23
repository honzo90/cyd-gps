/* LVGL 9.2 for the CYD on Arduino (no PSRAM: keep the heap small). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (40U * 1024U)  // ~70% used incl. POMIAR; QR overlay needs ~5 KB more; rest of RAM: Wi-Fi + TLS
#define LV_DEF_REFR_PERIOD 20
#define LV_USE_LOG 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_32 1  // trip distance
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_QRCODE 1  // QR login on the Wi-Fi screen (~3 KB of LVGL heap only while shown)

#endif
