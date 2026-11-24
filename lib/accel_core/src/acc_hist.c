#include "acc_hist.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const char *acc_hist_header(void)
{
    return "id,utc,t0_50,t0_100,t0_200,t100_200,t100m,tquarter,v100m,vquarter,vmax,dist_m,dur_s,grade_pct,"
           "climb_m,aborted,low_rate,rate_hz\n";
}

// appends ",<value>" or "," (unknown); keeps going after an overflow, checked once at the end
static size_t put(char *buf, size_t n, size_t len, bool known, const char *fmt, double v)
{
    int w = known ? snprintf(len < n ? buf + len : NULL, len < n ? n - len : 0, fmt, v)
                  : snprintf(len < n ? buf + len : NULL, len < n ? n - len : 0, ",");
    return len + (w > 0 ? (size_t)w : 0);
}

size_t acc_hist_line(char *buf, size_t n, const char *id, const char *utc, const acc_result_t *r, float rate_hz)
{
    int w = snprintf(buf, n, "%s,%s", id, utc);
    size_t len = w > 0 ? (size_t)w : 0;
    for (int i = 0; i < ACC_N; ++i) {
        len = put(buf, n, len, r->t[i] > 0, ",%.2f", r->t[i]);
    }
    len = put(buf, n, len, r->t[ACC_T_100M] > 0, ",%.1f", r->v_100m);
    len = put(buf, n, len, r->t[ACC_T_QUARTER] > 0, ",%.1f", r->v_quarter);
    len = put(buf, n, len, true, ",%.1f", r->v_max);
    len = put(buf, n, len, true, ",%.1f", r->dist_m);
    len = put(buf, n, len, true, ",%.2f", r->duration_s);
    len = put(buf, n, len, r->grade_valid, ",%.1f", r->grade_pct);
    len = put(buf, n, len, r->grade_valid, ",%.1f", r->climb_m);
    len = put(buf, n, len, true, ",%.0f", r->aborted ? 1 : 0);
    len = put(buf, n, len, true, ",%.0f", r->low_rate ? 1 : 0);
    len = put(buf, n, len, true, ",%.1f\n", rate_hz);
    if (len >= n) {
        if (n) {
            buf[0] = 0;
        }
        return 0;
    }
    return len;
}

static bool digits(const char *s, int k)
{
    for (int i = 0; i < k; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

bool acc_hist_valid_id(const char *id)
{
    if (!id) {
        return false;
    }
    const size_t l = strlen(id);
    if (l == 15) {
        return digits(id, 8) && id[8] == '-' && digits(id + 9, 6);
    }
    return l == 7 && id[0] == 'R' && digits(id + 1, 6);
}

bool acc_hist_counts(const acc_result_t *r)
{
    return !r->aborted && !r->low_rate;
}

void acc_stats_add(acc_stats_t *s, const acc_result_t *r)
{
    ++s->runs;
    if (!acc_hist_counts(r)) {
        return;
    }
    for (int i = 0; i < ACC_N; ++i) {
        if (r->t[i] > 0 && s->n[i] < UINT16_MAX) {
            ++s->n[i];
            s->sum[i] += r->t[i];
            s->sq[i] += r->t[i] * r->t[i];
        }
    }
}

float acc_stats_mean(const acc_stats_t *s, int i)
{
    return s->n[i] ? s->sum[i] / s->n[i] : -1;
}

float acc_stats_sd(const acc_stats_t *s, int i)
{
    if (s->n[i] < 2) {
        return -1;
    }
    const double m = (double)s->sum[i] / s->n[i];
    const double var = ((double)s->sq[i] - s->n[i] * m * m) / (s->n[i] - 1);
    return var > 0 ? (float)sqrt(var) : 0;
}

// next comma-separated field; *p advances past the comma. Empty field -> def.
static float field(const char **p, float def, bool *ok)
{
    const char *s = *p;
    const char *e = strchr(s, ',');
    const char *end = e ? e : s + strcspn(s, "\r\n");
    float v = def;
    if (end > s) {
        char tmp[24];
        size_t l = (size_t)(end - s);
        if (l >= sizeof tmp) {
            *ok = false;
            l = sizeof tmp - 1;
        }
        memcpy(tmp, s, l);
        tmp[l] = 0;
        char *stop;
        v = strtof(tmp, &stop);
        if (*stop) {
            *ok = false;
        }
    }
    *p = e ? e + 1 : end;
    return v;
}

bool acc_hist_parse(const char *line, acc_hist_row_t *row)
{
    const char *c = strchr(line, ',');
    if (!c || (size_t)(c - line) >= sizeof row->id) {
        return false;
    }
    memcpy(row->id, line, (size_t)(c - line));
    row->id[c - line] = 0;
    if (!acc_hist_valid_id(row->id)) {
        return false;
    }
    const char *p = strchr(c + 1, ',');  // skip utc
    if (!p) {
        return false;
    }
    ++p;
    bool ok = true;
    for (int i = 0; i < ACC_N; ++i) {
        row->t[i] = field(&p, -1, &ok);
    }
    field(&p, 0, &ok);  // v100m
    field(&p, 0, &ok);  // vquarter
    row->vmax = field(&p, 0, &ok);
    field(&p, 0, &ok);  // dist
    field(&p, 0, &ok);  // duration
    row->grade_pct = field(&p, NAN, &ok);
    field(&p, 0, &ok);  // climb
    row->aborted = field(&p, 0, &ok) != 0;
    row->low_rate = field(&p, 0, &ok) != 0;
    return ok;
}

int acc_hist_tail(const char *buf, size_t len, bool from_start, acc_hist_row_t *rows, int max)
{
    // line starts, scanned backwards from the end
    int n = 0;
    size_t end = len;
    while (n < max && end > 0) {
        size_t s = end;
        while (s > 0 && buf[s - 1] != '\n') {
            --s;
        }
        if (s == 0 && !from_start) {
            break;  // first line of a partial buffer: may be cut
        }
        char line[256];
        size_t l = end - s;
        if (l > 0 && l < sizeof line) {
            memcpy(line, buf + s, l);
            line[l] = 0;
            if (acc_hist_parse(line, &rows[n])) {
                ++n;
            }
        }
        if (s == 0) {
            break;
        }
        end = s - 1;
    }
    return n;
}

static int days_in_month(int y, int m)
{
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}

// 0 = Sunday (Sakamoto)
static int weekday(int y, int m, int d)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int last_sunday(int y, int m)
{
    const int last = days_in_month(y, m);
    return last - weekday(y, m, last);
}

void acc_hist_when(const char *id, char *out, size_t n)
{
    if (!acc_hist_valid_id(id) || id[0] == 'R') {
        snprintf(out, n, "nr %d", id && id[0] == 'R' ? atoi(id + 1) : 0);
        return;
    }
    int y = atoi((char[]){id[0], id[1], id[2], id[3], 0});
    int mo = atoi((char[]){id[4], id[5], 0}), d = atoi((char[]){id[6], id[7], 0});
    int h = atoi((char[]){id[9], id[10], 0}), mi = atoi((char[]){id[11], id[12], 0});
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo)) {
        snprintf(out, n, "?");
        return;
    }
    // summer time in UTC terms: [last Sun March 01:00, last Sun October 01:00)
    const int key = mo * 10000 + d * 100 + h;
    const bool summer = key >= 3 * 10000 + last_sunday(y, 3) * 100 + 1 && key < 10 * 10000 + last_sunday(y, 10) * 100 + 1;
    h += summer ? 2 : 1;
    if (h >= 24) {  // next day
        h -= 24;
        if (++d > days_in_month(y, mo)) {
            d = 1;
            if (++mo > 12) {
                mo = 1;
            }
        }
    }
    snprintf(out, n, "%02d.%02d %02d:%02d", d, mo, h, mi);
}
