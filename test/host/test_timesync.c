/* Host test: network time, UTC to the unit's local clock with daylight saving, and the "is it worth correcting" decision. */
#include <stdio.h>
#include <string.h>

#include "dst.h"
#include "timesync.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static mvhr_clock_t clk(int y, int mo, int d, int h, int mi)
{
    mvhr_clock_t c;
    memset(&c, 0, sizeof(c));
    c.year = (uint8_t)(y - 2000);
    c.month = (uint8_t)mo;
    c.day = (uint8_t)d;
    c.hour = (uint8_t)h;
    c.minute = (uint8_t)mi;
    return c;
}

static bool same(const mvhr_clock_t *c, int y, int mo, int d, int h, int mi)
{
    return c->year == y - 2000 && c->month == mo && c->day == d && c->hour == h && c->minute == mi;
}

int main(void)
{
    mvhr_clock_t c;

    /* epoch to clock */
    timesync_epoch_to_clock(946684800, 0, &c);                       /* 2000-01-01 00:00 UTC */
    CHECK(same(&c, 2000, 1, 1, 0, 0));
    timesync_epoch_to_clock(1793893284, 0, &c);                      /* 2026-11-05 15:41:24 UTC */
    CHECK(same(&c, 2026, 11, 5, 15, 41));
    timesync_epoch_to_clock(1793893284, 60, &c);                     /* +1 h */
    CHECK(same(&c, 2026, 11, 5, 16, 41));
    timesync_epoch_to_clock(1793893284, -300, &c);                   /* -5 h */
    CHECK(same(&c, 2026, 11, 5, 10, 41));
    timesync_epoch_to_clock(1798761599, 60, &c);                     /* 2026-12-31 23:59:59 UTC, +1 h: new year */
    CHECK(same(&c, 2027, 1, 1, 0, 59));
    timesync_epoch_to_clock(1835479800, 0, &c);                      /* 2028-02-29 23:30 UTC, leap day */
    CHECK(same(&c, 2028, 2, 29, 23, 30));
    timesync_epoch_to_clock(1835479800, 60, &c);
    CHECK(same(&c, 2028, 3, 1, 0, 30));
    CHECK(timesync_clock_minutes(&(mvhr_clock_t){.year = 0, .month = 1, .day = 1}) == 0);
    mvhr_clock_t a = clk(2026, 3, 1, 0, 0), b = clk(2026, 2, 28, 23, 0);
    CHECK(timesync_clock_minutes(&a) - timesync_clock_minutes(&b) == 60);

    timesync_plan_t p;

    /* UK (UTC+0): the target follows summer time exactly at the changeover */
    mvhr_clock_t unit = clk(2026, 3, 29, 0, 59);
    timesync_plan(1774745970, 0, DST_RULE_UK, &unit, &p);      /* 00:59:30 UTC */
    CHECK(!p.summer && same(&p.target, 2026, 3, 29, 0, 59) && !p.write);
    unit = clk(2026, 3, 29, 1, 0);
    timesync_plan(1774746000, 0, DST_RULE_UK, &unit, &p);      /* 01:00:00 UTC: clocks go to 02:00 */
    CHECK(p.summer && same(&p.target, 2026, 3, 29, 2, 0));
    CHECK(p.diff_min == -60 && p.write);                             /* the unit still shows 01:00 */
    unit = clk(2026, 10, 25, 1, 59);
    timesync_plan(1792889940, 0, DST_RULE_UK, &unit, &p);      /* 00:59 UTC: still summer, 01:59 */
    CHECK(p.summer && same(&p.target, 2026, 10, 25, 1, 59) && !p.write);
    timesync_plan(1792890000, 0, DST_RULE_UK, &unit, &p);      /* 01:00 UTC: winter, 01:00 */
    CHECK(!p.summer && same(&p.target, 2026, 10, 25, 1, 0) && p.diff_min == 59 && p.write);

    /* Central Europe (UTC+1): 01:00 UTC on the last Sunday of March is 03:00 */
    unit = clk(2026, 3, 29, 2, 0);
    timesync_plan(1774746000, 60, DST_RULE_CENTRAL, &unit, &p);
    CHECK(p.summer && same(&p.target, 2026, 3, 29, 3, 0) && p.write);

    /* US Eastern (UTC-5): 07:00 UTC on 8 March is 03:00 EDT; 06:00 UTC on 1 November is 01:00 EST */
    unit = clk(2026, 3, 8, 1, 59);
    timesync_plan(1772953140, -300, DST_RULE_US, &unit, &p);   /* 06:59 UTC = 01:59 EST */
    CHECK(!p.summer && same(&p.target, 2026, 3, 8, 1, 59) && !p.write);
    timesync_plan(1772953200, -300, DST_RULE_US, &unit, &p);   /* 07:00 UTC = 03:00 EDT */
    CHECK(p.summer && same(&p.target, 2026, 3, 8, 3, 0) && p.write);
    unit = clk(2026, 11, 1, 1, 59);
    timesync_plan(1793512740, -300, DST_RULE_US, &unit, &p);   /* 05:59 UTC = 01:59 EDT */
    CHECK(p.summer && same(&p.target, 2026, 11, 1, 1, 59) && !p.write);
    timesync_plan(1793512800, -300, DST_RULE_US, &unit, &p);   /* 06:00 UTC = 01:00 EST */
    CHECK(!p.summer && same(&p.target, 2026, 11, 1, 1, 0) && p.write);

    /* tolerance: up to 2 minutes is left alone, more is corrected */
    mvhr_clock_t now = clk(2026, 11, 5, 15, 41);                     /* 15:41 UTC, winter, UK */
    for (int off = -4; off <= 4; off++) {
        unit = now;
        unit.minute = (uint8_t)(41 + off);
        timesync_plan(1793893284, 0, DST_RULE_UK, &unit, &p);
        CHECK(p.write == (off < -2 || off > 2));
        CHECK(p.diff_min == off);
    }

    /* no daylight saving rule: standard time all year, the unit's hour is corrected like the minutes */
    unit = clk(2026, 6, 15, 12, 0);                                  /* 12:00 UTC in June, UTC+0, no rule */
    timesync_plan(1781524800, 0, DST_RULE_NONE, &unit, &p);
    CHECK(!p.summer && p.diff_min == 0 && !p.write && same(&p.target, 2026, 6, 15, 12, 0));
    unit = clk(2026, 6, 15, 13, 0);                                  /* an hour fast */
    timesync_plan(1781524800, 0, DST_RULE_NONE, &unit, &p);
    CHECK(!p.summer && p.diff_min == 60 && p.write && same(&p.target, 2026, 6, 15, 12, 0));
    timesync_plan(1781524800, 0, DST_RULE_UK, &unit, &p);            /* with the UK rule 13:00 is right in June */
    CHECK(p.summer && p.diff_min == 0 && !p.write);

    printf(fails ? "%d FAILED\n" : "all timesync tests passed\n", fails);
    return fails != 0;
}
