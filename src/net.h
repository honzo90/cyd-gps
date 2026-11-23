// Wi-Fi for the CYD: home network (STA) when configured and in range, own hotspot (AP) otherwise.
// Credentials and the device password live in NVS ("net"), never in the source.
// Device password: random on first boot, shown on the Wi-Fi screen; it is the hotspot password
// and the web login (user "cyd").
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fw_version.h"

struct net_status_t {
    bool enabled;
    bool sta_configured;
    bool sta_connected;
    bool sta_auth_failed;  // home network rejected the password: retries stopped
    bool ap_on;
    uint8_t ap_clients;
    int8_t rssi;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ssid[20];
    char ap_ip[16];
    char dev_pass[12];
    bool ntp_ok;
};

void net_begin();
void net_poll();                       // from loop(): reconnects, hotspot fallback, OTA confirmation
void net_get(net_status_t *s);
void net_set_enabled(bool on);         // saved in NVS
void net_set_home(const char *ssid, const char *pass);  // saved in NVS, reconnects (call from loop task)
void net_forget_home();
bool net_ntp_time(char *buf, size_t n);  // "HH:MM:SS" UTC when NTP synced
const char *net_user();
const char *net_password();

// Requests from the web task, executed by net_poll() in the loop task.
void net_request_home(const char *ssid, const char *pass);
void net_request_forget();
void net_request_reboot();             // after OTA: closes the GPX file, saves the odometer, restarts
