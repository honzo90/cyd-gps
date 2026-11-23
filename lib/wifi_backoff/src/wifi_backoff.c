#include "wifi_backoff.h"

/* Values of ESP-IDF's WIFI_REASON_* (esp_wifi_types.h, identical in IDF 4.4 and 5.1).
 * wifi_sta.c checks them with _Static_assert against the real headers. */
enum {
    R_AUTH_EXPIRE = 2,
    R_AUTH_LEAVE = 3,
    R_ASSOC_EXPIRE = 4,
    R_ASSOC_TOOMANY = 5,
    R_ASSOC_LEAVE = 8,
    R_MIC_FAILURE = 14,
    R_4WAY_HANDSHAKE_TIMEOUT = 15,
    R_802_1X_AUTH_FAILED = 23,
    R_BEACON_TIMEOUT = 200,
    R_NO_AP_FOUND = 201,
    R_AUTH_FAIL = 202,
    R_ASSOC_FAIL = 203,
    R_HANDSHAKE_TIMEOUT = 204,
    R_CONNECTION_FAIL = 205,
};

void wifi_backoff_init(wifi_backoff_t *b, uint32_t base_ms, uint32_t max_ms)
{
    b->base_ms = base_ms ? base_ms : 1;
    b->max_ms = max_ms < b->base_ms ? b->base_ms : max_ms;
    wifi_backoff_reset(b);
}

void wifi_backoff_reset(wifi_backoff_t *b)
{
    b->attempt = 0;
    b->cred_failures = 0;
}

wifi_fail_class_t wifi_reason_class(uint8_t reason)
{
    switch (reason) {
    case R_NO_AP_FOUND:
        return WIFI_FAIL_AP_NOT_FOUND;
    case R_AUTH_FAIL:
    case R_4WAY_HANDSHAKE_TIMEOUT:
    case R_HANDSHAKE_TIMEOUT:
    case R_802_1X_AUTH_FAILED:
    case R_MIC_FAILURE:
        return WIFI_FAIL_CREDENTIALS;
    default:
        return WIFI_FAIL_TRANSIENT;
    }
}

const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case R_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case R_AUTH_LEAVE: return "AUTH_LEAVE";
    case R_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case R_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case R_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case R_MIC_FAILURE: return "MIC_FAILURE";
    case R_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case R_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case R_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case R_NO_AP_FOUND: return "NO_AP_FOUND";
    case R_AUTH_FAIL: return "AUTH_FAIL";
    case R_ASSOC_FAIL: return "ASSOC_FAIL";
    case R_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case R_CONNECTION_FAIL: return "CONNECTION_FAIL";
    default: return "OTHER";
    }
}

uint32_t wifi_backoff_on_failure(wifi_backoff_t *b, uint8_t reason, uint32_t rnd)
{
    if (wifi_reason_class(reason) == WIFI_FAIL_CREDENTIALS) {
        if (++b->cred_failures >= WIFI_BACKOFF_CRED_LIMIT) {
            return 0;
        }
    } else {
        b->cred_failures = 0;
    }

    uint32_t shift = b->attempt < 31 ? b->attempt : 31;
    uint64_t d = (uint64_t)b->base_ms << shift;
    if (d > b->max_ms) {
        d = b->max_ms;
    }
    b->attempt++;

    uint64_t half = d / 2;
    uint64_t delay = half + (uint64_t)rnd % (d - half + 1);  /* equal jitter: [d/2, d] */
    return delay ? (uint32_t)delay : 1;
}
