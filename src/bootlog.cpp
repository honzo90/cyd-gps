#include "bootlog.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// RTC slow memory, not cleared by a restart (software, panic, watchdog, brownout); random after a
// power loss, hence the magic and the check value.
struct rtc_rec_t {
    uint32_t magic;
    uint32_t uptime_s;   // last known uptime of this run
    uint32_t wdt;        // != 0: a task watchdog fired; task names below
    char cpu0[16], cpu1[16];
    uint32_t check;
};
static RTC_NOINIT_ATTR rtc_rec_t s_rtc;
static const uint32_t k_magic = 0xC1D60B07;

static uint32_t rtc_check(const rtc_rec_t &r)
{
    uint32_t h = 2166136261u;  // FNV-1a over everything before `check`
    const uint8_t *p = (const uint8_t *)&r;
    for (size_t i = 0; i < offsetof(rtc_rec_t, check); ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static uint32_t s_count;
static uint8_t s_last[BOOTLOG_N];   // newest first, esp_reset_reason_t values (0 = none)
static uint32_t s_up[BOOTLOG_N];    // uptime (s) of the run that ended with that restart; ~0 = unknown
static char s_wdt[40];              // tasks of the last watchdog reset, "CPU0 web, CPU1 loopTask"

// Called by ESP-IDF from the task watchdog interrupt, just before the panic restart. The task
// that starved the idle task of CPU 0 is the one running there now.
extern "C" void esp_task_wdt_isr_user_handler(void)
{
    rtc_rec_t r = s_rtc;
    r.magic = k_magic;
    r.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    r.wdt = 1;
    const char *n0 = pcTaskGetName(xTaskGetCurrentTaskHandleForCPU(0));
    const char *n1 = pcTaskGetName(xTaskGetCurrentTaskHandleForCPU(1));
    strncpy(r.cpu0, n0 ? n0 : "?", sizeof r.cpu0 - 1);
    r.cpu0[sizeof r.cpu0 - 1] = 0;
    strncpy(r.cpu1, n1 ? n1 : "?", sizeof r.cpu1 - 1);
    r.cpu1[sizeof r.cpu1 - 1] = 0;
    r.check = rtc_check(r);
    s_rtc = r;
}

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
    const bool rtc_ok = s_rtc.magic == k_magic && s_rtc.check == rtc_check(s_rtc);
    // a power loss wipes RTC memory: the previous run's length is then unknown
    const uint32_t prev_up = rtc_ok && r != ESP_RST_POWERON ? s_rtc.uptime_s : UINT32_MAX;
    char wdt[40] = "";
    if (rtc_ok && s_rtc.wdt && r == ESP_RST_TASK_WDT) {
        snprintf(wdt, sizeof wdt, "CPU0 %s, CPU1 %s", s_rtc.cpu0, s_rtc.cpu1);
    }
    Preferences p;
    if (p.begin("sys", false)) {
        s_count = p.getUInt("boots", 0) + 1;
        p.getBytes("last", s_last, sizeof s_last);
        if (p.getBytes("up", s_up, sizeof s_up) != sizeof s_up) {
            memset(s_up, 0xFF, sizeof s_up);  // older firmware: unknown
        }
        memmove(s_last + 1, s_last, BOOTLOG_N - 1);
        memmove(s_up + 1, s_up, sizeof s_up - sizeof s_up[0]);
        s_last[0] = r;
        s_up[0] = prev_up;
        p.putUInt("boots", s_count);
        p.putBytes("last", s_last, sizeof s_last);
        p.putBytes("up", s_up, sizeof s_up);
        if (wdt[0]) {
            p.putString("wdt", wdt);
        }
        p.getString("wdt", s_wdt, sizeof s_wdt);
        p.end();
    } else {
        s_last[0] = r;
        s_up[0] = prev_up;
    }
    // this run starts now
    rtc_rec_t n = {};
    n.magic = k_magic;
    n.check = rtc_check(n);
    s_rtc = n;
    Serial.printf("BOOT: #%lu, reason %u (%s)", (unsigned long)s_count, r, name(r));
    if (prev_up != UINT32_MAX) {
        Serial.printf(", previous run %lu s", (unsigned long)prev_up);
    }
    Serial.println();
    if (wdt[0]) {
        Serial.printf("BOOT: watchdog: %s\n", wdt);
    }
}

void bootlog_tick()
{
    const uint32_t up = millis() / 1000;
    if (up != s_rtc.uptime_s) {
        s_rtc.uptime_s = up;
        s_rtc.check = rtc_check(s_rtc);
    }
}

// " po 12 min" / " po 45 s" / "" (unknown)
static void fmt_up(char *b, size_t n, uint32_t s)
{
    if (s == UINT32_MAX) {
        b[0] = 0;
    } else if (s < 120) {
        snprintf(b, n, " po %lu s", (unsigned long)s);
    } else {
        snprintf(b, n, " po %lu min", (unsigned long)(s / 60));
    }
}

void bootlog_print()
{
    Serial.printf("BOOT: %lu starts; last (newest first):", (unsigned long)s_count);
    for (int i = 0; i < BOOTLOG_N && s_last[i]; ++i) {
        char up[20];
        fmt_up(up, sizeof up, s_up[i]);
        Serial.printf(" %s%s;", name(s_last[i]), up);
    }
    Serial.println();
    if (s_wdt[0]) {
        Serial.printf("BOOT: last task watchdog: %s\n", s_wdt);
    }
}

void bootlog_text(char *buf, size_t n)
{
    int len = snprintf(buf, n, "Uruchomienia: %lu. Ostatnie przyczyny: ", (unsigned long)s_count);
    for (int i = 0; i < BOOTLOG_N && s_last[i] && len > 0 && (size_t)len < n; ++i) {
        char up[20];
        fmt_up(up, sizeof up, s_up[i]);
        len += snprintf(buf + len, n - len, "%s%s%s", i ? ", " : "", name(s_last[i]), up);
    }
    if (s_wdt[0] && len > 0 && (size_t)len < n) {
        snprintf(buf + len, n - len, ". Ostatni watchdog: %s", s_wdt);
    }
}
