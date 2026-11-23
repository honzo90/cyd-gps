#include "bootlog.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>

static uint32_t s_count;
static uint8_t s_last[BOOTLOG_N];  // newest first, esp_reset_reason_t values (0 = none)

static const char *name(uint8_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "zasilanie wl.";      // power applied (or the power bank cut out)
    case ESP_RST_BROWNOUT: return "BROWNOUT";          // supply dipped below ~2.4 V
    case ESP_RST_PANIC: return "BLAD PROGRAMU";        // panic / abort
    case ESP_RST_INT_WDT: return "watchdog przerw.";
    case ESP_RST_TASK_WDT: return "watchdog zadania";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_SW: return "restart programowy";      // e.g. after an update
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "przycisk/zewn.";
    case 0: return "";
    default: return "inny";
    }
}

void bootlog_begin()
{
    const uint8_t r = (uint8_t)esp_reset_reason();
    Preferences p;
    if (p.begin("sys", false)) {
        s_count = p.getUInt("boots", 0) + 1;
        p.getBytes("last", s_last, sizeof s_last);
        memmove(s_last + 1, s_last, BOOTLOG_N - 1);
        s_last[0] = r;
        p.putUInt("boots", s_count);
        p.putBytes("last", s_last, sizeof s_last);
        p.end();
    } else {
        s_last[0] = r;
    }
    Serial.printf("BOOT: #%lu, reason %u (%s)\n", (unsigned long)s_count, r, name(r));
}

void bootlog_print()
{
    Serial.printf("BOOT: %lu starts; last (newest first):", (unsigned long)s_count);
    for (int i = 0; i < BOOTLOG_N && s_last[i]; ++i) {
        Serial.printf(" %s;", name(s_last[i]));
    }
    Serial.println();
}

void bootlog_text(char *buf, size_t n)
{
    int len = snprintf(buf, n, "Uruchomienia: %lu. Ostatnie przyczyny: ", (unsigned long)s_count);
    for (int i = 0; i < BOOTLOG_N && s_last[i] && len > 0 && (size_t)len < n; ++i) {
        len += snprintf(buf + len, n - len, "%s%s", i ? ", " : "", name(s_last[i]));
    }
}
