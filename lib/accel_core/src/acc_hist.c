#include "acc_hist.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
