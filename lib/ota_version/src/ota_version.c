#include "ota_version.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    unsigned long part[3];
    const char *pre;   /* points after '-', NULL if none */
    size_t pre_len;
} ver_t;

static bool parse_num(const char **p, unsigned long *out)
{
    if (!isdigit((unsigned char)**p)) {
        return false;
    }
    unsigned long v = 0;
    while (isdigit((unsigned char)**p)) {
        unsigned d = (unsigned)(**p - '0');
        if (v > (999999999UL - d) / 10) {
            return false;  /* absurdly long number */
        }
        v = v * 10 + d;
        (*p)++;
    }
    *out = v;
    return true;
}

static bool parse(const char *s, ver_t *v)
{
    if (s == NULL) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int i = 0; i < 3; i++) {
        if (!parse_num(&s, &v->part[i])) {
            return false;
        }
        if (i < 2 && *s++ != '.') {
            return false;
        }
    }
    v->pre = NULL;
    v->pre_len = 0;
    if (*s == '-') {
        v->pre = ++s;
        while (*s && *s != '+') {
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') {
                return false;
            }
            s++;
        }
        v->pre_len = (size_t)(s - v->pre);
        if (v->pre_len == 0) {
            return false;
        }
    }
    if (*s == '+') {
        s++;
        if (*s == '\0') {
            return false;
        }
        while (*s) {  /* build metadata: ignored for ordering */
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

ota_version_cmp_t ota_version_compare(const char *offered, const char *running)
{
    ver_t a, b;
    if (!parse(offered, &a) || !parse(running, &b)) {
        return OTA_VERSION_INVALID;
    }
    for (int i = 0; i < 3; i++) {
        if (a.part[i] != b.part[i]) {
            return a.part[i] > b.part[i] ? OTA_VERSION_NEWER : OTA_VERSION_OLDER;
        }
    }
    if (!a.pre && !b.pre) {
        return OTA_VERSION_SAME;
    }
    if (!a.pre) {
        return OTA_VERSION_NEWER;  /* 1.0.0 > 1.0.0-rc1 */
    }
    if (!b.pre) {
        return OTA_VERSION_OLDER;
    }
    /* both prereleases: plain byte order of the identifier (rc1 < rc2; rc10 < rc2, keep rc01..) */
    size_t n = a.pre_len < b.pre_len ? a.pre_len : b.pre_len;
    int c = memcmp(a.pre, b.pre, n);
    if (c == 0 && a.pre_len != b.pre_len) {
        c = a.pre_len > b.pre_len ? 1 : -1;
    }
    return c > 0 ? OTA_VERSION_NEWER : c < 0 ? OTA_VERSION_OLDER : OTA_VERSION_SAME;
}

bool ota_version_should_install(const char *offered, const char *running, bool allow_downgrade)
{
    ota_version_cmp_t c = ota_version_compare(offered, running);
    return c == OTA_VERSION_NEWER || (allow_downgrade && c == OTA_VERSION_OLDER);
}
