#pragma once
/* Daylight saving rules (pure C, host-testable). Used by network time (timesync.h) to know whether summer time is on
   at a given moment. The controller never moves the unit's clock by itself on these dates. */
#include <stdbool.h>

#include "mvhr.h"

typedef enum {
    DST_RULE_UK,        /* UK, Ireland, Portugal: last Sunday of March 01:00 -> 02:00, last Sunday of October 02:00 -> 01:00 */
    DST_RULE_CENTRAL,   /* Central Europe: last Sunday of March 02:00 -> 03:00, last Sunday of October 03:00 -> 02:00 */
    DST_RULE_EAST,      /* Eastern Europe: last Sunday of March 03:00 -> 04:00, last Sunday of October 04:00 -> 03:00 */
    DST_RULE_US,        /* US, Canada: second Sunday of March 02:00 -> 03:00, first Sunday of November 02:00 -> 01:00 */
    DST_RULE_COUNT
} dst_rule_t;

#define DST_RULE_NONE   DST_RULE_COUNT      /* no daylight saving: standard time all year */

const char *dst_rule_name(int rule);

/* Add (or subtract) whole hours, carrying through day, month and year. */
void dst_shift_hours(mvhr_clock_t *c, int hours);

/* Move forward (+1), back (-1) or leave alone (0). active = the clock currently includes summer time. */
int dst_decide(int rule, const mvhr_clock_t *c, bool active);

/* Day of month of the changeover Sundays for a year (full year, e.g. 2026): for tests and the status text. */
int dst_start_day(int rule, int year);
int dst_end_day(int rule, int year);
