/*
 * Decide whether an offered firmware version should be installed.
 * Versions: "MAJOR.MINOR.PATCH", optional leading 'v', optional "-prerelease" / "+build" suffix
 * (e.g. "v1.4.0-rc2+abc"). A prerelease is older than the same version without one.
 * Pure C: host-testable, shared by the ESP-IDF and Arduino templates.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_VERSION_INVALID = -2,  /* either string does not parse: never update on it */
    OTA_VERSION_OLDER = -1,    /* offered < running */
    OTA_VERSION_SAME = 0,
    OTA_VERSION_NEWER = 1,     /* offered > running */
} ota_version_cmp_t;

ota_version_cmp_t ota_version_compare(const char *offered, const char *running);

/* true only for NEWER, or for OLDER when allow_downgrade. SAME is false: prevents the
 * classic "update, reboot, same version offered again, update again" loop. */
bool ota_version_should_install(const char *offered, const char *running, bool allow_downgrade);

#ifdef __cplusplus
}
#endif
