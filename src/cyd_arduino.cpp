#include "cyd_arduino.h"

#include <Arduino.h>
#include <Preferences.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/semphr.h"
#include "xpt2046.h"

// Pins: witnessmenow/ESP32-Cheap-Yellow-Display PINS.md + DisplayConfig/CYD2USB/User_Setup.h
static const int PIN_LCD_SCLK = 14, PIN_LCD_MOSI = 13, PIN_LCD_MISO = 12, PIN_LCD_CS = 15, PIN_LCD_DC = 2;
static const int PIN_LCD_BL = 21;
static const int PIN_TP_CLK = 25, PIN_TP_MOSI = 32, PIN_TP_MISO = 39, PIN_TP_CS = 33;
static const int PIN_LED_R = 4, PIN_LED_G = 16, PIN_LED_B = 17;
static const int BL_CH = 0;           // LEDC channel (Arduino 2.0 API)
static const int DRAW_LINES = 12;  // 2 x 7.7 KB: leaves contiguous heap for TLS (2 x 16.7 KB buffers)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static cyd_cal_t s_cal;

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *, const lv_area_t *a, uint8_t *px)
{
    lv_draw_sw_rgb565_swap(px, lv_area_get_size(a));
    xSemaphoreTake(s_flush_done, 0);
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static void flush_wait_cb(lv_display_t *)
{
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("cyd: SPI flush timeout");
    }
}

// ---- touch, bit-banged ----
static void tp_cs(void *, int l) { digitalWrite(PIN_TP_CS, l); }
static void tp_clk(void *, int l) { digitalWrite(PIN_TP_CLK, l); }
static void tp_mosi(void *, int l) { digitalWrite(PIN_TP_MOSI, l); }
static int tp_miso(void *) { return digitalRead(PIN_TP_MISO); }
static void tp_delay(void *) { delayMicroseconds(1); }
static const xpt2046_bus_t s_tp = {tp_cs, tp_clk, tp_mosi, tp_miso, tp_delay, nullptr};

bool cyd_touch_raw(uint16_t *rx, uint16_t *ry)
{
    xpt2046_raw_t r;
    if (!xpt2046_read(&s_tp, XPT2046_Z_THRESHOLD, &r)) {
        return false;
    }
    *rx = r.x;
    *ry = r.y;
    return true;
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data)
{
    static lv_point_t last;
    uint16_t rx, ry;
    if (cyd_touch_raw(&rx, &ry)) {
        int16_t x, y;
        cyd_cal_apply(&s_cal, rx, ry, CYD_H_RES, CYD_V_RES, &x, &y);
        last.x = x;
        last.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point = last;
}

void cyd_touch_set_cal(const cyd_cal_t *cal) { s_cal = *cal; }

bool cyd_cal_load(cyd_cal_t *cal)
{
    Preferences p;
    if (!p.begin("cyd", true)) {
        return false;
    }
    size_t n = p.getBytes("cal", cal, sizeof *cal);
    p.end();
    return n == sizeof *cal && cyd_cal_valid(cal);
}

bool cyd_cal_save(const cyd_cal_t *cal)
{
    Preferences p;
    if (!p.begin("cyd", false)) {
        return false;
    }
    size_t n = p.putBytes("cal", cal, sizeof *cal);
    p.end();
    return n == sizeof *cal;
}

void cyd_led(bool r, bool g, bool b)
{
    digitalWrite(PIN_LED_R, !r);  // active low
    digitalWrite(PIN_LED_G, !g);
    digitalWrite(PIN_LED_B, !b);
}

void cyd_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    esp_lcd_panel_disp_on_off(s_panel, percent > 0);
    ledcWrite(BL_CH, 1023u * percent / 100);
}

bool cyd_begin(uint32_t spi_hz)
{
    s_flush_done = xSemaphoreCreateBinary();
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    cyd_led(false, false, false);
    ledcSetup(BL_CH, 5000, 10);
    ledcAttachPin(PIN_LCD_BL, BL_CH);
    ledcWrite(BL_CH, 0);
    pinMode(PIN_TP_CS, OUTPUT);
    digitalWrite(PIN_TP_CS, HIGH);
    pinMode(PIN_TP_CLK, OUTPUT);
    digitalWrite(PIN_TP_CLK, LOW);
    pinMode(PIN_TP_MOSI, OUTPUT);
    pinMode(PIN_TP_MISO, INPUT);

    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = PIN_LCD_MISO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = CYD_H_RES * DRAW_LINES * 2;
    if (spi_bus_initialize(HSPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        Serial.println("cyd: HSPI busy? (do not also start TFT_eSPI or SPI.begin on HSPI)");
        return false;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.dc_gpio_num = PIN_LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = spi_hz;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_trans_done;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = -1;
    dev.color_space = CYD_LCD_BGR ? ESP_LCD_COLOR_SPACE_BGR : ESP_LCD_COLOR_SPACE_RGB;  // IDF 4.4 name
    dev.bits_per_pixel = 16;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HSPI_HOST, &io_cfg, &io) != ESP_OK ||
        esp_lcd_new_panel_st7789(io, &dev, &s_panel) != ESP_OK || esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK) {
        Serial.println("cyd: panel init failed");
        return false;
    }
    esp_lcd_panel_invert_color(s_panel, CYD_LCD_INVERT);
    esp_lcd_panel_swap_xy(s_panel, true);  // landscape
#ifndef CYD_LCD_MIRROR_X
#define CYD_LCD_MIRROR_X 1
#endif
#ifndef CYD_LCD_MIRROR_Y
#define CYD_LCD_MIRROR_Y 0
#endif
    esp_lcd_panel_mirror(s_panel, CYD_LCD_MIRROR_X, CYD_LCD_MIRROR_Y);
    cyd_cal_default(&s_cal, CYD_H_RES, CYD_V_RES);

    const size_t bytes = CYD_H_RES * DRAW_LINES * 2;
    void *b1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) {
        Serial.println("cyd: no DMA RAM for draw buffers");
        return false;
    }
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    lv_display_t *disp = lv_display_create(CYD_H_RES, CYD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_flush_wait_cb(disp, flush_wait_cb);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);
    lv_indev_set_display(indev, disp);
    return true;
}

void cyd_loop()
{
    uint32_t wait = lv_timer_handler();
    delay(wait > 10 ? 10 : (wait ? wait : 1));
}
