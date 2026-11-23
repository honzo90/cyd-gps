#include "gps.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>

static TinyGPSPlus s_gps;
static HardwareSerial &s_uart = Serial2;
static bool s_echo;

void gps_toggle_echo()
{
    s_echo = !s_echo;
}

// ---- Satellite signal (C/N0) from GSV sentences ------------------------------------------
// TinyGPSPlus does not parse GSV, so complete NMEA lines are checked and parsed here.
// One entry per satellite (talker + PRN); entries older than 3 s count as gone.
struct sat_t {
    char sys[3];     // talker: GP=GPS, GL=GLONASS, BD/GB=BeiDou, GA=Galileo
    uint16_t prn;
    uint8_t elev;    // degrees
    uint8_t snr;     // C/N0 dB-Hz, 0 = in view but not tracked
    uint32_t ms;     // last seen
};
static const int k_max_sats = 64;
static sat_t s_sat[k_max_sats];
static const uint32_t k_sat_ttl_ms = 3000;

static int nmea_field_int(const char *f, const char *end)
{
    return (f < end && *f != ',' && *f != '*') ? atoi(f) : -1;
}

static void sat_update(const char *sys, int prn, int elev, int snr, uint32_t now)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < k_max_sats; ++i) {
        if (s_sat[i].prn == prn && s_sat[i].sys[0] == sys[0] && s_sat[i].sys[1] == sys[1]) {
            slot = i;
            break;
        }
        if (s_sat[i].ms < s_sat[oldest].ms) {
            oldest = i;
        }
    }
    sat_t &e = s_sat[slot >= 0 ? slot : oldest];
    e.sys[0] = sys[0];
    e.sys[1] = sys[1];
    e.sys[2] = 0;
    e.prn = (uint16_t)prn;
    e.elev = elev < 0 ? 0 : (uint8_t)elev;
    e.snr = snr < 0 ? 0 : (uint8_t)snr;
    e.ms = now;
}

// line: "$xxGSV,total,num,inview,prn,elev,az,snr,...*CS" without CR/LF.
// NMEA 4.1 may append a signal-ID field after the last group; it is ignored.
static void parse_line(const char *line)
{
    if (line[0] != '$') {
        return;
    }
    const char *star = strchr(line, '*');
    if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) {
        return;
    }
    uint8_t cs = 0;
    for (const char *p = line + 1; p < star; ++p) {
        cs ^= (uint8_t)*p;
    }
    if (cs != (uint8_t)strtoul(star + 1, NULL, 16) || strncmp(line + 3, "GSV,", 4) != 0) {
        return;
    }
    const char sys[2] = {line[1], line[2]};
    const char *f[24];  // field starts, empty fields kept
    int n = 0;
    for (const char *p = line; p && n < 24;) {
        f[n++] = p;
        p = strchr(p, ',');
        p = (p && p < star) ? p + 1 : NULL;
    }
    const uint32_t now = millis();
    for (int i = 4; i + 3 < n; i += 4) {
        const int prn = nmea_field_int(f[i], star);
        if (prn > 0) {
            sat_update(sys, prn, nmea_field_int(f[i + 1], star), nmea_field_int(f[i + 3], star), now);
        }
    }
}

static char s_line[100];
static int s_line_len;
static uint32_t s_fix_seq, s_fix_ms;

static void feed(char c)
{
    // A completed sentence that moved the position = a new fix (GGA and RMC each count once).
    if (s_gps.encode(c) && s_gps.location.isUpdated() && s_gps.location.isValid()) {
        s_gps.location.lat();  // reading clears the "updated" flag, so the next sentence does not count again
        s_fix_ms = millis();
        ++s_fix_seq;
    }
    if (c == '$') {
        s_line_len = 0;
    }
    if (c == '\r' || c == '\n') {
        if (s_line_len > 0) {
            s_line[s_line_len] = 0;
            parse_line(s_line);
            s_line_len = 0;
        }
    } else if (s_line_len < (int)sizeof s_line - 1) {
        s_line[s_line_len++] = c;
    } else {
        s_line_len = 0;  // too long: not an NMEA line
    }
}

static void sat_stats(uint8_t *view, uint8_t *tracked, uint8_t *strong, uint8_t *max, uint8_t *top4)
{
    const uint32_t now = millis();
    uint8_t best[4] = {0, 0, 0, 0};
    *view = *tracked = *strong = *max = 0;
    for (const sat_t &e : s_sat) {
        if (!e.prn || now - e.ms > k_sat_ttl_ms) {
            continue;
        }
        ++*view;
        if (!e.snr) {
            continue;
        }
        ++*tracked;
        *strong += e.snr >= 30;
        uint8_t v = e.snr;
        for (uint8_t &b : best) {  // keep the 4 largest, descending
            if (v > b) {
                const uint8_t t = b;
                b = v;
                v = t;
            }
        }
    }
    *max = best[0];
    int sum = 0, cnt = 0;
    for (uint8_t b : best) {
        if (b) {
            sum += b;
            ++cnt;
        }
    }
    *top4 = cnt ? (uint8_t)(sum / cnt) : 0;
}

// Every 2 s one line on USB serial, plus a line whenever the fix is gained or lost, e.g.
// SIG t=63s fix=1 used=7 hdop=1.3 view=14 trk=9 >=30:5 max=41 top4=38 | GP08:41 GP27:39 BD06:33 ...
static void sig_log()
{
    static uint32_t last;
    static bool last_fix;
    const uint32_t now = millis();
    const bool fix = s_gps.location.isValid() && s_gps.location.age() < 3000;
    if (fix != last_fix) {
        Serial.printf("SIG t=%lus FIX %s\n", (unsigned long)(now / 1000), fix ? "ZDOBYTY" : "UTRACONY");
        last_fix = fix;
    }
    if (now - last < 2000) {
        return;
    }
    last = now;
    uint8_t view, trk, strong, mx, top4;
    sat_stats(&view, &trk, &strong, &mx, &top4);
    Serial.printf("SIG t=%lus fix=%d used=%lu hdop=%.1f view=%u trk=%u >=30:%u max=%u top4=%u |",
                  (unsigned long)(now / 1000), fix,
                  (unsigned long)(s_gps.satellites.isValid() ? s_gps.satellites.value() : 0),
                  s_gps.hdop.isValid() ? s_gps.hdop.hdop() : 0.0, view, trk, strong, mx, top4);
    bool printed[k_max_sats] = {};
    for (;;) {  // tracked satellites, strongest first
        int bi = -1;
        for (int i = 0; i < k_max_sats; ++i) {
            const sat_t &e = s_sat[i];
            if (!printed[i] && e.prn && e.snr && now - e.ms <= k_sat_ttl_ms && (bi < 0 || e.snr > s_sat[bi].snr)) {
                bi = i;
            }
        }
        if (bi < 0) {
            break;
        }
        printed[bi] = true;
        Serial.printf(" %s%02u:%u", s_sat[bi].sys, s_sat[bi].prn, s_sat[bi].snr);
    }
    Serial.print("\n");
}

#ifdef GPS_SIM
// Synthetic GGA+RMC once per second (valid checksums), slowly moving position. No UART used.
static void sim_sentence(const char *body)
{
    uint8_t cs = 0;
    for (const char *p = body; *p; ++p) {
        cs ^= (uint8_t)*p;
    }
    char line[128];
    snprintf(line, sizeof line, "$%s*%02X\r\n", body, cs);
    for (const char *p = line; *p; ++p) {
        feed(*p);
    }
}

static void sim_tick()
{
    static uint32_t last, n;
    if (millis() - last < 1000) {
        return;
    }
    last = millis();
    ++n;
    const uint32_t t = n % 86400;
    const unsigned hh = t / 3600, mm = t / 60 % 60, ss = t % 60;
    const double lat = 52.2297 + n * 0.00002, lon = 21.0122 + n * 0.00003;  // drift ~3 m/s
    const int lat_d = (int)lat, lon_d = (int)lon;
    const double lat_m = (lat - lat_d) * 60, lon_m = (lon - lon_d) * 60;
    char body[100];
    snprintf(body, sizeof body, "GPGGA,%02u%02u%02u.00,%02d%08.5f,N,%03d%08.5f,E,1,%02u,0.9,112.4,M,34.5,M,,",
             hh, mm, ss, lat_d, lat_m, lon_d, lon_m, 7 + (unsigned)(n / 5 % 5));
    sim_sentence(body);
    snprintf(body, sizeof body, "GPRMC,%02u%02u%02u.00,A,%02d%08.5f,N,%03d%08.5f,E,6.5,45.0,231126,,,A",
             hh, mm, ss, lat_d, lat_m, lon_d, lon_m);
    sim_sentence(body);
}
#endif

// Auto-baud: the module's baud rate may have been changed and saved (clones often ship at 38400/115200).
// Try each rate for a window; lock on the first one that yields NMEA with valid checksums.
// Re-scan if a locked rate stops producing valid sentences for a while.
static const uint32_t k_bauds[] = {GPS_BAUD, 9600, 38400, 57600, 19200, 4800, 230400};  // GPS_BAUD first
static const int k_nbauds = sizeof k_bauds / sizeof k_bauds[0];
static int s_baud_idx;
static bool s_locked;
static uint32_t s_window_start, s_window_pass, s_last_pass_ms;

static void set_baud(int idx)
{
    s_baud_idx = idx;
    s_uart.updateBaudRate(k_bauds[idx]);
    while (s_uart.available()) {
        s_uart.read();
    }
    s_window_start = millis();
    s_window_pass = s_gps.passedChecksum();
    Serial.printf("GPS: trying %lu baud\n", (unsigned long)k_bauds[idx]);
}

static void autobaud_step()
{
    const uint32_t now = millis();
    if (s_gps.passedChecksum() != s_window_pass) {
        s_last_pass_ms = now;
        if (!s_locked && s_gps.passedChecksum() - s_window_pass >= 2) {
            s_locked = true;
            Serial.printf("GPS: locked at %lu baud\n", (unsigned long)k_bauds[s_baud_idx]);
        }
        s_window_pass = s_gps.passedChecksum();
    }
    if (s_locked) {
        if (now - s_last_pass_ms > 5000) {  // lost it: start scanning again
            s_locked = false;
            set_baud((s_baud_idx + 1) % k_nbauds);
        }
    } else if (now - s_window_start > 2500) {  // NMEA comes at least once per second
        set_baud((s_baud_idx + 1) % k_nbauds);
    }
}

void gps_begin()
{
#ifdef GPS_SIM
    return;
#endif
    // LVGL redraws can keep loop() busy for tens of ms; 4 KB covers >0.3 s even at 115200 baud.
    s_uart.setRxBufferSize(4096);
    s_uart.begin(k_bauds[0], SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    set_baud(0);
}

void gps_poll()
{
#ifdef GPS_SIM
    sim_tick();
    return;
#endif
    while (s_uart.available()) {
        const char c = (char)s_uart.read();
        feed(c);
        if (s_echo) {
            Serial.write(c);
        }
    }
    autobaud_step();
    if (s_locked) {
        sig_log();
    }
}

uint32_t gps_fix_seq()
{
    return s_fix_seq;
}

void gps_get(gps_data_t *o)
{
    o->fix_seq = s_fix_seq;
    o->fix_ms = s_fix_ms;
    o->fix = s_gps.location.isValid() && s_gps.location.age() < 3000;
    o->lat = s_gps.location.lat();
    o->lon = s_gps.location.lng();
    o->alt_m = s_gps.altitude.isValid() ? s_gps.altitude.meters() : 0;
    o->speed_kmh = s_gps.speed.isValid() ? s_gps.speed.kmph() : 0;
    o->course_deg = s_gps.course.isValid() ? s_gps.course.deg() : 0;
    o->sats = s_gps.satellites.isValid() ? s_gps.satellites.value() : 0;
    o->hdop = s_gps.hdop.isValid() ? s_gps.hdop.hdop() : 0;
    o->time_valid = s_gps.time.isValid();
    o->hh = s_gps.time.hour();
    o->mm = s_gps.time.minute();
    o->ss = s_gps.time.second();
    o->date_valid = s_gps.date.isValid() && s_gps.date.year() > 2000;
    o->year = s_gps.date.year();
    o->month = s_gps.date.month();
    o->day = s_gps.date.day();
    o->chars = s_gps.charsProcessed();
    o->bad_sum = s_gps.failedChecksum();
    sat_stats(&o->sat_view, &o->sat_tracked, &o->sat_strong, &o->snr_max, &o->snr_top4);
#ifdef GPS_SIM
    o->baud = 0;
    o->baud_locked = true;
#else
    o->baud = k_bauds[s_baud_idx];
    o->baud_locked = s_locked;
#endif
}
