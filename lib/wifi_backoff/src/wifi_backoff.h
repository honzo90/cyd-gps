/*
 * Reconnect policy for Wi-Fi STA: exponential backoff with jitter, and a stop
 * condition when the access point keeps rejecting the credentials.
 * Pure C, no ESP-IDF headers: builds on the host for unit tests and in Arduino.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_FAIL_TRANSIENT,     /* weak signal, AP restart, roaming, beacon loss: retry with backoff */
    WIFI_FAIL_AP_NOT_FOUND,  /* SSID not seen: wrong name, 5 GHz-only network, out of range, channel 12/13 */
    WIFI_FAIL_CREDENTIALS,   /* password/auth rejected: retrying forever will not help */
} wifi_fail_class_t;

/* Consecutive credential-class failures after which wifi_backoff_on_failure() returns 0.
 * Handshake timeouts are also caused by a weak signal, hence more than one. */
#define WIFI_BACKOFF_CRED_LIMIT 3

typedef struct {
    uint32_t base_ms;        /* first retry delay */
    uint32_t max_ms;         /* cap */
    uint32_t attempt;        /* failures since the last successful connection */
    uint32_t cred_failures;  /* consecutive WIFI_FAIL_CREDENTIALS failures */
} wifi_backoff_t;

void wifi_backoff_init(wifi_backoff_t *b, uint32_t base_ms, uint32_t max_ms);

/* Call on successful connection (got IP). */
void wifi_backoff_reset(wifi_backoff_t *b);

/* Record one disconnect with its 802.11 / ESP-IDF reason code. Returns the delay in ms
 * before the next connect attempt, in [d/2, d] where d = base * 2^attempt capped at max,
 * or 0 = stop retrying (credentials rejected WIFI_BACKOFF_CRED_LIMIT times in a row).
 * rnd: any random 32-bit value (esp_random() on the chip, fixed in tests). */
uint32_t wifi_backoff_on_failure(wifi_backoff_t *b, uint8_t reason, uint32_t rnd);

wifi_fail_class_t wifi_reason_class(uint8_t reason);

/* Short name of the ESP-IDF WIFI_REASON_* constant, "OTHER" if not listed. */
const char *wifi_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
