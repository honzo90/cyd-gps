#include "gpx_log.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>
#include "gpx_core.h"

#define SD_SS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

static const uint32_t k_flush_ms = 5000;  // card write at most every 5 s (wear, and a slow card blocks loop())

static SPIClass s_spi(VSPI);  // the LCD owns HSPI; touch is bit-banged
static bool s_mounted, s_error;
static File s_file;
static char s_name[32];
static uint32_t s_points;
static size_t s_body_end;  // file offset where the closing tags start
static gpx_last_t s_last;
static char s_pending[2048];  // points not yet on the card
static size_t s_pending_len;
static uint32_t s_last_flush;

static bool mount()
{
    s_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_SS);
    s_mounted = SD.begin(SD_SS, s_spi, 10000000);
    if (s_mounted) {
        if (!SD.exists("/gpx")) {
            SD.mkdir("/gpx");
        }
        Serial.printf("SD: %s, %llu MB\n", SD.cardType() == CARD_SDHC ? "SDHC" : "SD", SD.cardSize() >> 20);
    } else {
        Serial.println("SD: no card");
    }
    s_error = false;
    return s_mounted;
}

static void fail(const char *what)
{
    Serial.printf("SD: %s failed, logging stopped\n", what);
    if (s_file) {
        s_file.close();
    }
    s_error = true;
    s_mounted = false;
    s_pending_len = 0;
}

static void flush_pending()
{
    if (!s_file || !s_pending_len) {
        return;
    }
    // Overwrite the old closing tags with the new points, then write the closing tags again.
    if (!s_file.seek(s_body_end) || s_file.write((const uint8_t *)s_pending, s_pending_len) != s_pending_len) {
        fail("write");
        return;
    }
    s_body_end = s_file.position();
    if (s_file.print(GPX_FOOTER) != strlen(GPX_FOOTER)) {
        fail("write");
        return;
    }
    s_file.flush();
    s_pending_len = 0;
    s_last_flush = millis();
}

static void close_file()
{
    if (s_file) {
        flush_pending();
        s_file.close();
        Serial.printf("SD: closed %s, %lu points\n", s_name, (unsigned long)s_points);
    }
    memset(&s_last, 0, sizeof s_last);
}

static bool open_file(const gpx_pt_t *p)
{
    char path[40], head[256];
    gpx_filename(path, sizeof path, p);
    s_file = SD.open(path, FILE_WRITE);
    if (!s_file) {
        fail("open");
        return false;
    }
    snprintf(s_name, sizeof s_name, "%s", path + 5);  // without "/gpx/"
    char track[24];
    snprintf(track, sizeof track, "%.15s", s_name);    // name without ".gpx"
    const int n = gpx_header(head, sizeof head, track);
    if (s_file.write((const uint8_t *)head, n) != (size_t)n) {
        fail("write");
        return false;
    }
    s_body_end = s_file.position();
    s_file.print(GPX_FOOTER);
    s_file.flush();
    s_points = 0;
    s_pending_len = 0;
    s_last_flush = millis();
    Serial.printf("SD: logging to %s\n", path);
    return true;
}

void gpx_log_begin()
{
    mount();
}

void gpx_log_feed(const gps_data_t *g)
{
    if (!s_mounted || !g->fix || !g->time_valid || !g->date_valid) {
        return;
    }
    const gpx_decision_t d = gpx_decide(&s_last, g->lat, g->lon, (float)g->hdop, (uint8_t)g->sats,
                                         (float)g->speed_kmh, g->fix_ms);
    if (d == GPX_SKIP) {
        return;
    }
    const gpx_pt_t p = {g->lat, g->lon, (float)g->alt_m, g->alt_m != 0, g->year, g->month, g->day,
                        g->hh,  g->mm,  g->ss,           (uint8_t)g->sats, (float)g->hdop, (float)g->speed_kmh};
    if (!s_file && !open_file(&p)) {
        return;
    }
    char line[320];
    int n = 0;
    if (d == GPX_LOG_NEW_SEG && s_points > 0) {
        n = snprintf(line, sizeof line, "%s", GPX_NEW_SEG);
    }
    n += gpx_point(line + n, sizeof line - n, &p);
    if (s_pending_len + n >= sizeof s_pending) {
        flush_pending();
    }
    memcpy(s_pending + s_pending_len, line, n);
    s_pending_len += n;
    ++s_points;
}

void gpx_log_poll()
{
    if (s_file && s_pending_len && millis() - s_last_flush >= k_flush_ms) {
        flush_pending();
    }
}

void gpx_log_new_track()
{
    close_file();
    if (!s_mounted) {
        mount();  // card inserted later, or after an error
    }
}

uint32_t gpx_log_current(char *name, size_t n)
{
    snprintf(name, n, "%s", s_file ? s_name : "");
    return s_file ? s_points : 0;
}

void gpx_log_status(char *buf, size_t n)
{
    if (s_error) {
        snprintf(buf, n, "SD: blad zapisu - wloz karte, zeruj trip");
    } else if (!s_mounted) {
        snprintf(buf, n, "SD: brak karty");
    } else if (!s_file) {
        snprintf(buf, n, "SD: czekam na dobry fix");
    } else {
        snprintf(buf, n, "SD: %s  %lu pkt", s_name, (unsigned long)s_points);
    }
}

bool gpx_log_valid_name(const char *n)
{
    const size_t len = strlen(n);
    if (len < 5 || len > 30 || strcmp(n + len - 4, ".gpx") != 0) {
        return false;
    }
    for (size_t i = 0; i < len - 4; ++i) {
        if (!isdigit((unsigned char)n[i]) && n[i] != '-' && n[i] != '_') {
            return false;
        }
    }
    return true;
}

void gpx_log_print_files()
{
    if (!s_mounted) {
        Serial.println("FILES: no SD card");
        return;
    }
    File dir = SD.open("/gpx");
    if (!dir) {
        Serial.println("FILES: no /gpx directory");
        return;
    }
    unsigned count = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char *name = strrchr(f.name(), '/') ? strrchr(f.name(), '/') + 1 : f.name();
        if (!f.isDirectory() && gpx_log_valid_name(name)) {
            const bool active = s_file && strcmp(name, s_name) == 0;
            Serial.printf("FILES: %s %lu B%s\n", name, (unsigned long)f.size(), active ? " (writing)" : "");
            ++count;
        }
        f.close();
    }
    dir.close();
    Serial.printf("FILES: %u file(s), card %llu MB used of %llu MB\n", count, SD.usedBytes() >> 20,
                  SD.totalBytes() >> 20);
}

const char *gpx_log_delete(const char *name)
{
    if (!gpx_log_valid_name(name)) {
        return "bad name (expected e.g. 20261123-114602.gpx)";
    }
    if (!s_mounted) {
        return "no SD card";
    }
    if (s_file && strcmp(name, s_name) == 0) {
        return "file is being written (reset the trip to close it first)";
    }
    char path[40];
    snprintf(path, sizeof path, "/gpx/%s", name);
    if (!SD.exists(path)) {
        return "not found";
    }
    return SD.remove(path) ? "ok" : "remove failed";
}
