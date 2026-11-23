#include "gh_ota.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <string.h>
#include "fw_version.h"
#include "net.h"
#include "ota_version.h"

#ifndef GH_REPO
#error "GH_REPO (owner/name) must be set in platformio.ini"
#endif

static const char k_version_url[] = "https://github.com/" GH_REPO "/releases/latest/download/version.txt";
static const char k_firmware_url[] = "https://github.com/" GH_REPO "/releases/latest/download/firmware.bin";
static const uint32_t k_first_check_ms = 30000;           // after the home network is up
static const uint32_t k_check_every_ms = 6UL * 3600 * 1000;

// Root CAs, embedded by board_build.embed_txtfiles (NUL-terminated).
extern const char gh_roots_pem_start[] asm("_binary_src_gh_roots_pem_start");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gh_status_t s_st;
static TaskHandle_t s_task;
static volatile bool s_req_check, s_req_install;

static void set_state(gh_state_t st, const char *err = nullptr)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = st;
    snprintf(s_st.error, sizeof s_st.error, "%s", err ? err : "");
    portEXIT_CRITICAL(&s_mux);
}

static bool online()
{
    return WiFi.status() == WL_CONNECTED;  // home network (the hotspot has no internet)
}

static void check()
{
    set_state(GH_CHECKING);
    WiFiClientSecure tls;
    tls.setCACert(gh_roots_pem_start);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // github.com -> release-assets host
    http.setUserAgent("cyd-gps/" FW_VERSION);
    http.setTimeout(15000);
    if (!http.begin(tls, k_version_url)) {
        set_state(GH_FAILED, "polaczenie");
        return;
    }
    const int code = http.GET();
    if (code == 404) {
        http.end();
        set_state(GH_NO_RELEASE);
        Serial.println("GH: no release yet");
        return;
    }
    if (code != 200) {
        char e[48];
        if (code < 0) {
            snprintf(e, sizeof e, "%s", http.errorToString(code).c_str());  // TLS, DNS, timeout
        } else {
            snprintf(e, sizeof e, "HTTP %d", code);
        }
        http.end();
        set_state(GH_FAILED, e);
        Serial.printf("GH: check failed: %s\n", e);
        return;
    }
    String v = http.getString();
    http.end();
    v.trim();
    if (v.length() == 0 || v.length() >= sizeof s_st.latest) {
        set_state(GH_FAILED, "zly version.txt");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.latest, sizeof s_st.latest, "%s", v.c_str());
    portEXIT_CRITICAL(&s_mux);
    const ota_version_cmp_t c = ota_version_compare(v.c_str(), FW_VERSION);
    if (c == OTA_VERSION_INVALID) {
        set_state(GH_FAILED, "zly numer wersji");
    } else {
        set_state(c == OTA_VERSION_NEWER ? GH_AVAILABLE : GH_UP_TO_DATE);
    }
    Serial.printf("GH: latest %s, running %s -> %s\n", v.c_str(), FW_VERSION,
                  c == OTA_VERSION_NEWER ? "update available" : "nothing to do");
}

static bool running_image_pending()
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void install()
{
    if (running_image_pending()) {
        set_state(GH_FAILED, "poprzednia aktualizacja niepotwierdzona");
        return;
    }
    if (Update.isRunning()) {
        set_state(GH_FAILED, "trwa inna aktualizacja");
        return;
    }
    set_state(GH_INSTALLING);
    Serial.printf("GH: installing %s from %s\n", s_st.latest, k_firmware_url);
    WiFiClientSecure tls;
    tls.setCACert(gh_roots_pem_start);
    tls.setTimeout(20);
    httpUpdate.rebootOnUpdate(false);  // the loop task closes the GPX file and saves the odometer first
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpUpdate.onProgress([](int done, int total) {
        if (total > 0) {
            s_st.progress = (uint8_t)((int64_t)done * 100 / total);
        }
    });
    const HTTPUpdateResult r = httpUpdate.update(tls, k_firmware_url);
    if (r == HTTP_UPDATE_OK) {
        Serial.println("GH: firmware written, restarting");
        net_request_reboot();
        return;
    }
    char e[64];
    snprintf(e, sizeof e, "%s", httpUpdate.getLastErrorString().c_str());
    Serial.printf("GH: install failed: %s\n", e);
    set_state(GH_FAILED, e);
}

static void task(void *)
{
    uint32_t online_since = 0, last_check = 0;
    bool checked_once = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        const uint32_t now = millis();
        if (!online()) {
            online_since = 0;
            s_req_install = false;
            continue;
        }
        if (!online_since) {
            online_since = now;
        }
        if (s_req_install) {
            s_req_install = false;
            if (s_st.state == GH_AVAILABLE) {
                install();
            }
            continue;
        }
        const bool due = (!checked_once && now - online_since > k_first_check_ms) ||
                         (checked_once && now - last_check > k_check_every_ms);
        if (s_req_check || due) {
            s_req_check = false;
            checked_once = true;
            last_check = now;
            check();
        }
    }
}

void gh_ota_begin()
{
    // TLS handshake + HTTP: mbedTLS takes most of its memory from the heap, the rest is stack.
    xTaskCreatePinnedToCore(task, "gh_ota", 10240, NULL, 1, &s_task, 0);
}

void gh_ota_check_now()
{
    s_req_check = true;
}

void gh_ota_install()
{
    s_req_install = true;
}

bool gh_ota_busy()
{
    return s_st.state == GH_INSTALLING;
}

void gh_ota_get(gh_status_t *s)
{
    portENTER_CRITICAL(&s_mux);
    *s = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void gh_ota_text(char *buf, size_t n)
{
    gh_status_t s;
    gh_ota_get(&s);
    switch (s.state) {
    case GH_IDLE:
        snprintf(buf, n, "GitHub: jeszcze nie sprawdzono");
        break;
    case GH_CHECKING:
        snprintf(buf, n, "GitHub: sprawdzam...");
        break;
    case GH_UP_TO_DATE:
        snprintf(buf, n, "GitHub: aktualna (v%s)", FW_VERSION);
        break;
    case GH_AVAILABLE:
        snprintf(buf, n, "GitHub: dostepna %s", s.latest);
        break;
    case GH_NO_RELEASE:
        snprintf(buf, n, "GitHub: brak wydan");
        break;
    case GH_INSTALLING:
        snprintf(buf, n, "GitHub: instaluje %s  %u%%", s.latest, s.progress);
        break;
    case GH_FAILED:
        snprintf(buf, n, "GitHub: blad - %s", s.error);
        break;
    }
}
