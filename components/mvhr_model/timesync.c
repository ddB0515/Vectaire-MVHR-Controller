#include "timesync.h"

#include <stdlib.h>
#include <string.h>

#include "dst.h"

/* days since 1970-01-01 for a civil date, and back (H. Hinnant's algorithms) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

void timesync_epoch_to_clock(int64_t utc, int tz_std_min, mvhr_clock_t *c)
{
    int64_t t = utc + (int64_t)tz_std_min * 60;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t sec = t - days * 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    memset(c, 0, sizeof(*c));
    c->year = (uint8_t)(y - 2000);
    c->month = (uint8_t)m;
    c->day = (uint8_t)d;
    c->hour = (uint8_t)(sec / 3600);
    c->minute = (uint8_t)(sec % 3600 / 60);
}

int64_t timesync_clock_minutes(const mvhr_clock_t *c)
{
    return (days_from_civil(2000 + c->year, c->month, c->day) - days_from_civil(2000, 1, 1)) * 1440 +
           c->hour * 60 + c->minute;
}

void timesync_plan(int64_t utc, int tz_std_min, int dst_rule, const mvhr_clock_t *unit, timesync_plan_t *out)
{
    mvhr_clock_t std;
    timesync_epoch_to_clock(utc, tz_std_min, &std);
    int64_t std_min = timesync_clock_minutes(&std);
    int64_t unit_min = timesync_clock_minutes(unit);
    bool summer = dst_rule != DST_RULE_NONE && dst_decide(dst_rule, &std, false) > 0;   /* standard time inside the summer period */
    out->summer = summer;
    out->target = std;
    if (summer) {
        dst_shift_hours(&out->target, 1);
    }
    out->diff_min = (int)(unit_min - (std_min + (summer ? 60 : 0)));
    out->write = abs(out->diff_min) > TIMESYNC_TOLERANCE_MIN;
}
