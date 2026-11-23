// Firmware updates from GitHub Releases (public repo GH_REPO, set in platformio.ini).
// Checks https://github.com/<repo>/releases/latest/download/version.txt after the home network
// comes up and every 6 h; installs firmware.bin from the same release only when asked
// (screen or web page) and only if it is newer. HTTPS with the server certificate verified
// against the roots in src/gh_roots.pem. Runs in its own task.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum gh_state_t { GH_IDLE, GH_CHECKING, GH_UP_TO_DATE, GH_AVAILABLE, GH_NO_RELEASE, GH_INSTALLING, GH_FAILED };

struct gh_status_t {
    gh_state_t state;
    char latest[24];   // version in the latest release ("" = unknown)
    uint8_t progress;  // % while installing
    char error[64];
};

void gh_ota_begin();
void gh_ota_check_now();
void gh_ota_install();   // only acts in GH_AVAILABLE
bool gh_ota_busy();      // downloading/writing: the web upload must not start
void gh_ota_get(gh_status_t *s);
void gh_ota_text(char *buf, size_t n);  // one line for screen and web page
