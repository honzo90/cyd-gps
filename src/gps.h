// NEO-8M (clone) on UART2, NMEA; baud auto-detected, 115200 tried first.
// Wiring to CYD connector CN1 (JST 1.25, 4 pins: GND, IO22, IO27, 3V3):
//   NEO-8M VCC -> 3V3, GND -> GND, TX -> IO27 (ESP RX), RX -> IO22 (ESP TX)
#pragma once
#include <stdint.h>

#ifndef GPS_RX_PIN
#define GPS_RX_PIN 27
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN 22
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 115200  // first rate tried; this module was found at 115200 (auto-baud covers others)
#endif

struct gps_data_t {
    bool fix;            // valid position, updated within the last 3 s
    double lat, lon;     // degrees
    double alt_m;
    double speed_kmh;
    double course_deg;
    uint32_t sats;
    double hdop;
    bool time_valid;
    uint8_t hh, mm, ss;  // UTC
    bool date_valid;
    uint16_t year;
    uint8_t month, day;
    uint32_t chars;      // NMEA bytes received in total (0 = check wiring)
    uint32_t bad_sum;    // sentences with a bad checksum (many = wrong baud rate)
    uint32_t baud;       // current UART rate (auto-detected)
    bool baud_locked;    // valid NMEA seen at this rate
    // Signal (C/N0, dB-Hz) from GSV. Rough guide: <25 weak, 30-40 good, >40 open sky.
    uint8_t sat_view;    // satellites the module lists as in view
    uint8_t sat_tracked; // ... of which it actually receives (C/N0 > 0)
    uint8_t sat_strong;  // ... with C/N0 >= 30
    uint8_t snr_max;
    uint8_t snr_top4;    // mean of the 4 strongest (what a fix depends on)
    uint32_t fix_seq;    // +1 for every new position (GGA and RMC both count)
    uint32_t fix_ms;     // millis() when the latest position arrived
};

void gps_begin();
void gps_poll();                    // call often from loop(): drains UART into the parser
void gps_get(gps_data_t *out);
uint32_t gps_fix_seq();
void gps_toggle_echo();             // raw NMEA copied to the USB serial (diagnostics, u-center)             // cheap check for "new position since last time"
