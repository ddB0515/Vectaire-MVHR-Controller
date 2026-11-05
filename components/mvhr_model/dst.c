#include "dst.h"

typedef struct {
    uint8_t start_month, start_nth, start_hour;     /* Sunday; nth 0 = the last one; hour = standard time, before the change */
    uint8_t end_month, end_nth, end_hour;           /* hour = summer time, before the change back */
    const char *name;
} rule_t;

static const rule_t s_rules[DST_RULE_COUNT] = {
    [DST_RULE_UK] = {3, 0, 1, 10, 0, 2, "UK & Ireland"},
    [DST_RULE_CENTRAL] = {3, 0, 2, 10, 0, 3, "Central Europe"},
    [DST_RULE_EAST] = {3, 0, 3, 10, 0, 4, "Eastern Europe"},
    [DST_RULE_US] = {3, 2, 2, 11, 1, 2, "US & Canada"},
};

const char *dst_rule_name(int rule)
{
    return rule >= 0 && rule < DST_RULE_COUNT ? s_rules[rule].name : "None";
}

static int days_in_month(int year, int month)
{
    static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int n = d[month - 1];
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        n++;
    }
    return n;
}

static int weekday(int y, int m, int d)     /* 0 = Sunday (Sakamoto) */
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y--;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* nth Sunday of a month; nth 0 = the last one */
static int sunday(int year, int month, int nth)
{
    if (nth == 0) {
        int last = days_in_month(year, month);
        return last - weekday(year, month, last);
    }
    int first = 1 + (7 - weekday(year, month, 1)) % 7;
    return first + 7 * (nth - 1);
}

int dst_start_day(int rule, int year)
{
    const rule_t *r = &s_rules[rule];
    return sunday(year, r->start_month, r->start_nth);
}

int dst_end_day(int rule, int year)
{
    const rule_t *r = &s_rules[rule];
    return sunday(year, r->end_month, r->end_nth);
}

void dst_shift_hours(mvhr_clock_t *c, int hours)
{
    int year = 2000 + c->year;
    for (; hours > 0; hours--) {
        if (++c->hour == 24) {
            c->hour = 0;
            if (++c->day > days_in_month(year, c->month)) {
                c->day = 1;
                if (++c->month > 12) {
                    c->month = 1;
                    year++;
                }
            }
        }
    }
    for (; hours < 0; hours++) {
        if (c->hour == 0) {
            c->hour = 23;
            if (--c->day < 1) {
                if (--c->month < 1) {
                    c->month = 12;
                    year--;
                }
                c->day = (uint8_t)days_in_month(year, c->month);
            }
        } else {
            c->hour--;
        }
    }
    c->year = (uint8_t)(year - 2000);
}

/* minutes since the start of the year's month 1, comparable within one year (months are given 32 days) */
static int key(int month, int day, int hour, int minute)
{
    return ((month * 32 + day) * 24 + hour) * 60 + minute;
}

int dst_decide(int rule, const mvhr_clock_t *c, bool active)
{
    if (rule < 0 || rule >= DST_RULE_COUNT || c->month < 1 || c->month > 12 || c->day < 1 || c->day > 31 ||
        c->hour > 23 || c->minute > 59) {
        return 0;
    }
    const rule_t *r = &s_rules[rule];
    int year = 2000 + c->year;
    int start = key(r->start_month, sunday(year, r->start_month, r->start_nth), r->start_hour, 0);
    int end = key(r->end_month, sunday(year, r->end_month, r->end_nth), r->end_hour, 0);
    int now = key(c->month, c->day, c->hour, c->minute);
    if (!active) {
        /* standard time on the clock: summer time begins at "start"; the last hour before "end" is already over
           once the clock has gone back, so it counts as standard time again */
        return now >= start && now < end - 60 ? +1 : 0;
    }
    /* summer time on the clock: it runs from "start" + 1 h until "end" */
    return now >= start + 60 && now < end ? 0 : -1;
}
