#include "net.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <string.h>
#include <time.h>
#include "gpx_log.h"
#include "odometer.h"
#include "wifi_backoff.h"
#include "gh_ota.h"
#include "web.h"

static const uint32_t k_ap_fallback_ms = 20000;  // no home network for this long -> hotspot
static const uint32_t k_ap_off_ms = 10000;       // home network up and nobody on the hotspot -> hotspot off
static const uint32_t k_confirm_ms = 15000;      // a new OTA image must run this long with the network up

static Preferences s_nvs;
static bool s_enabled = true;
static char s_ssid[33], s_pass[65], s_dev_pass[12], s_ap_ssid[20];
static wifi_backoff_t s_backoff;

// Written by the Wi-Fi event task, read by the loop task.
static volatile bool s_got_ip, s_auth_failed;
static volatile uint32_t s_retry_at;  // 0 = no retry scheduled
static volatile uint32_t s_connected_at;

static bool s_ap_on, s_ntp_started, s_web_started, s_confirmed;
static uint32_t s_sta_started_at, s_no_clients_since;

// Requests from the web task.
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_req_home, s_req_forget, s_req_reboot;
static char s_req_ssid[33], s_req_pass[65];

static void make_password(char *out, size_t len)
{
    static const char k_chars[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no 0/o, 1/l/i
    for (size_t i = 0; i + 1 < len; ++i) {
        out[i] = k_chars[esp_random() % (sizeof k_chars - 1)];
    }
    out[len - 1] = 0;
}

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
    // Runs in the event task: never block or call WiFi.begin() here, only record and schedule.
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        wifi_backoff_reset(&s_backoff);
        s_auth_failed = false;
        s_retry_at = 0;
        s_connected_at = millis();
        s_got_ip = true;
        Serial.printf("WIFI: connected to home network, ip %s\n", WiFi.localIP().toString().c_str());
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        const uint32_t wait = wifi_backoff_on_failure(&s_backoff, reason, esp_random());
        s_got_ip = false;
        Serial.printf("WIFI: disconnected, reason %u %s, rssi %d, %s\n", reason, wifi_reason_name(reason),
                      info.wifi_sta_disconnected.rssi, wait ? "retry later" : "password rejected, stopped");
        if (wait) {
            s_retry_at = millis() + wait;
            if (!s_retry_at) {
                s_retry_at = 1;
            }
        } else {
            s_auth_failed = true;
            s_retry_at = 0;
        }
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        Serial.println("WIFI: hotspot client connected");
        break;
    default:
        break;
    }
}

static void start_ap()
{
    if (s_ap_on) {
        return;
    }
    WiFi.mode(s_ssid[0] ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(s_ap_ssid, s_dev_pass);
    s_ap_on = true;
    s_no_clients_since = 0;
    Serial.printf("WIFI: hotspot %s on, http://%s\n", s_ap_ssid, WiFi.softAPIP().toString().c_str());
}

static void stop_ap()
{
    if (!s_ap_on) {
        return;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap_on = false;
    Serial.println("WIFI: hotspot off (home network is up)");
}

static void start_sta()
{
    wifi_backoff_init(&s_backoff, 1000, 60000);
    s_auth_failed = false;
    s_retry_at = 0;
    s_got_ip = false;
    WiFi.mode(s_ap_on ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(s_ssid, s_pass);  // the password is never logged
    s_sta_started_at = millis();
    Serial.printf("WIFI: connecting to \"%s\"\n", s_ssid);
}

static void radio_on()
{
    WiFi.persistent(false);        // our own NVS keys, not the IDF's copy
    WiFi.setAutoReconnect(false);  // reconnects follow the backoff policy
    WiFi.setHostname("cyd-gps");
    if (s_ssid[0]) {
        start_sta();
    } else {
        start_ap();
    }
    if (!s_web_started) {
        web_begin();
        gh_ota_begin();
        s_web_started = true;
    }
}

static void radio_off()
{
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    s_ap_on = false;
    s_got_ip = false;
    s_retry_at = 0;
    Serial.println("WIFI: off");
}

void net_begin()
{
    s_nvs.begin("net", false);
    s_enabled = s_nvs.getBool("on", true);
    s_nvs.getString("ssid", s_ssid, sizeof s_ssid);
    s_nvs.getString("pass", s_pass, sizeof s_pass);
    if (s_nvs.getString("devpw", s_dev_pass, sizeof s_dev_pass) < 9) {
        make_password(s_dev_pass, 11);  // 10 characters, WPA2 needs >= 8
        s_nvs.putString("devpw", s_dev_pass);
    }
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "CYD-GPS-%02X%02X", mac[4], mac[5]);
    WiFi.onEvent(on_wifi_event);
    if (s_enabled) {
        radio_on();
    } else {
        WiFi.mode(WIFI_OFF);
    }
}

static void handle_requests()
{
    bool home, forget, reboot;
    char ssid[33], pass[65];
    portENTER_CRITICAL(&s_req_mux);
    home = s_req_home;
    forget = s_req_forget;
    reboot = s_req_reboot;
    memcpy(ssid, s_req_ssid, sizeof ssid);
    memcpy(pass, s_req_pass, sizeof pass);
    s_req_home = s_req_forget = s_req_reboot = false;
    memset(s_req_pass, 0, sizeof s_req_pass);
    portEXIT_CRITICAL(&s_req_mux);
    if (home) {
        net_set_home(ssid, pass);
    }
    memset(pass, 0, sizeof pass);
    if (forget) {
        net_forget_home();
    }
    if (reboot) {
        Serial.println("OTA: restarting into the new firmware");
        gpx_log_new_track();  // closes and flushes the GPX file
        odometer_save();
        delay(300);
        ESP.restart();
    }
}

// New OTA image: valid only after it has run for a while with the network and web server up,
// so a release that breaks Wi-Fi or the web page rolls back on its own.
static void confirm_image()
{
    if (s_confirmed || millis() < k_confirm_ms) {
        return;
    }
    if (s_enabled && !(s_web_started && (s_ap_on || s_got_ip))) {
        return;
    }
    s_confirmed = true;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("OTA: new image confirmed");
    }
}

void net_poll()
{
    handle_requests();
    confirm_image();
    if (!s_enabled) {
        return;
    }
    const uint32_t now = millis();
    if (s_ssid[0] && s_retry_at && (int32_t)(now - s_retry_at) >= 0) {
        s_retry_at = 0;
        WiFi.begin(s_ssid, s_pass);
    }
    // Home network missing: open the hotspot (STA keeps retrying alongside).
    if (!s_ap_on && !s_got_ip && (!s_ssid[0] || s_auth_failed || now - s_sta_started_at > k_ap_fallback_ms)) {
        start_ap();
    }
    // Home network up and the hotspot unused: switch it off.
    if (s_ap_on && s_got_ip && now - s_connected_at > k_ap_off_ms) {
        if (WiFi.softAPgetStationNum() == 0) {
            if (!s_no_clients_since) {
                s_no_clients_since = now;
            } else if (now - s_no_clients_since > k_ap_off_ms) {
                stop_ap();
            }
        } else {
            s_no_clients_since = 0;
        }
    }
    if (s_got_ip && !s_ntp_started) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        s_ntp_started = true;
    }
}

bool net_ntp_time(char *buf, size_t n)
{
    const time_t t = time(nullptr);
    if (t < 1700000000) {  // not synced yet
        return false;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, n, "%H:%M:%S", &tm);
    return true;
}

void net_get(net_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = s_enabled;
    s->sta_configured = s_ssid[0] != 0;
    s->sta_connected = s_got_ip;
    s->sta_auth_failed = s_auth_failed;
    s->ap_on = s_ap_on;
    snprintf(s->sta_ssid, sizeof s->sta_ssid, "%s", s_ssid);
    snprintf(s->ap_ssid, sizeof s->ap_ssid, "%s", s_ap_ssid);
    snprintf(s->dev_pass, sizeof s->dev_pass, "%s", s_dev_pass);
    if (s_got_ip) {
        snprintf(s->sta_ip, sizeof s->sta_ip, "%s", WiFi.localIP().toString().c_str());
        s->rssi = (int8_t)WiFi.RSSI();
    }
    if (s_ap_on) {
        snprintf(s->ap_ip, sizeof s->ap_ip, "%s", WiFi.softAPIP().toString().c_str());
        s->ap_clients = WiFi.softAPgetStationNum();
    }
    char tmp[12];
    s->ntp_ok = net_ntp_time(tmp, sizeof tmp);
}

void net_set_enabled(bool on)
{
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    s_nvs.putBool("on", on);
    if (on) {
        radio_on();
    } else {
        radio_off();
    }
}

void net_set_home(const char *ssid, const char *pass)
{
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    snprintf(s_pass, sizeof s_pass, "%s", pass);
    s_nvs.putString("ssid", s_ssid);
    s_nvs.putString("pass", s_pass);
    Serial.printf("WIFI: home network set to \"%s\"\n", s_ssid);
    if (s_enabled) {
        WiFi.disconnect(false);
        start_sta();  // the hotspot stays on until the home network is up
    }
}

void net_forget_home()
{
    s_ssid[0] = s_pass[0] = 0;
    s_nvs.remove("ssid");
    s_nvs.remove("pass");
    Serial.println("WIFI: home network forgotten");
    if (s_enabled) {
        WiFi.disconnect(false);
        s_got_ip = false;
        s_retry_at = 0;
        start_ap();
    }
}

const char *net_user()
{
    return "cyd";
}

const char *net_password()
{
    return s_dev_pass;
}

void net_request_home(const char *ssid, const char *pass)
{
    portENTER_CRITICAL(&s_req_mux);
    snprintf(s_req_ssid, sizeof s_req_ssid, "%s", ssid);
    snprintf(s_req_pass, sizeof s_req_pass, "%s", pass);
    s_req_home = true;
    portEXIT_CRITICAL(&s_req_mux);
}

void net_request_forget()
{
    portENTER_CRITICAL(&s_req_mux);
    s_req_forget = true;
    portEXIT_CRITICAL(&s_req_mux);
}

void net_request_reboot()
{
    portENTER_CRITICAL(&s_req_mux);
    s_req_reboot = true;
    portEXIT_CRITICAL(&s_req_mux);
}

// Arduino-ESP32 confirms every new image before setup() unless this returns true.
// Must be extern "C", otherwise the core's weak default stays active.
extern "C" bool verifyRollbackLater()
{
    return true;
}
