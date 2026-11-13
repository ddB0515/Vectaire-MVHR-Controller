#pragma once
/* Network time (pure C, host-testable): from a UTC time stamp, the time zone and the daylight saving rule, work out what
   the unit's clock should show and whether it is far enough off to be worth correcting. */
#include <stdbool.h>
#include <stdint.h>

#include "mvhr.h"

#define TIMESYNC_TOLERANCE_MIN  2       /* correct the clock only when it is more than this many minutes out */

typedef struct {
    bool write;                 /* the clock is out by more than the tolerance */
    bool summer;                /* the target includes summer time */
    int diff_min;               /* unit clock minus target, minutes */
    mvhr_clock_t target;        /* what the clock should show (the other fields are zero) */
} timesync_plan_t;

/* UTC seconds since 1970 to a local clock reading at a fixed offset from UTC (standard time, minutes). */
void timesync_epoch_to_clock(int64_t utc, int tz_std_min, mvhr_clock_t *c);

/* Minutes since 2000-01-01 00:00 of a clock reading, for differences. */
int64_t timesync_clock_minutes(const mvhr_clock_t *c);

/* dst_rule: a dst_rule_t, or DST_RULE_NONE for standard time all year. */
void timesync_plan(int64_t utc, int tz_std_min, int dst_rule, const mvhr_clock_t *unit, timesync_plan_t *out);
