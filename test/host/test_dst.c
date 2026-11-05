/* Host test: daylight saving rules and the clock shift, with a unit clock simulated minute by minute. */
#include <stdio.h>
#include <string.h>

#include "dst.h"

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

static void add_minute(mvhr_clock_t *c)
{
    if (++c->minute == 60) {
        c->minute = 0;
        dst_shift_hours(c, 1);
    }
}

typedef struct {
    int when_mo, when_d, when_h, when_mi;   /* clock reading just before the change */
    int dir;
} change_t;

/* Run a clock through [from, to) at one minute per step, applying every decision, and record the changes. */
static int run(int rule, mvhr_clock_t c, bool active, int steps, change_t *out, int max, bool *active_end)
{
    int n = 0;
    for (int i = 0; i < steps; i++) {
        add_minute(&c);
        int d = dst_decide(rule, &c, active);
        if (d) {
            if (n < max) {
                out[n] = (change_t){c.month, c.day, c.hour, c.minute, d};
            }
            n++;
            dst_shift_hours(&c, d);
            active = d > 0;
        }
    }
    *active_end = active;
    return n;
}

int main(void)
{
    /* changeover Sundays */
    CHECK(dst_start_day(DST_RULE_UK, 2026) == 29 && dst_end_day(DST_RULE_UK, 2026) == 25);
    CHECK(dst_start_day(DST_RULE_UK, 2027) == 28 && dst_end_day(DST_RULE_UK, 2027) == 31);
    CHECK(dst_start_day(DST_RULE_UK, 2024) == 31 && dst_end_day(DST_RULE_UK, 2024) == 27);
    CHECK(dst_start_day(DST_RULE_US, 2026) == 8 && dst_end_day(DST_RULE_US, 2026) == 1);     /* 8 March, 1 November */
    CHECK(dst_start_day(DST_RULE_US, 2027) == 14 && dst_end_day(DST_RULE_US, 2027) == 7);

    /* hour shifts carry through the day, month and year, and leap days */
    mvhr_clock_t c = clk(2026, 12, 31, 23, 30);
    dst_shift_hours(&c, 1);
    CHECK(c.year == 27 - 0 && c.month == 1 && c.day == 1 && c.hour == 0 && c.minute == 30);
    dst_shift_hours(&c, -1);
    CHECK(c.year == 26 && c.month == 12 && c.day == 31 && c.hour == 23);
    c = clk(2028, 2, 28, 23, 0);
    dst_shift_hours(&c, 1);
    CHECK(c.month == 2 && c.day == 29 && c.hour == 0);          /* 2028 is a leap year */
    c = clk(2027, 3, 1, 0, 0);
    dst_shift_hours(&c, -1);
    CHECK(c.month == 2 && c.day == 28 && c.hour == 23);
    c = clk(2026, 6, 15, 12, 0);
    dst_shift_hours(&c, 3);
    CHECK(c.hour == 15 && c.day == 15);

    /* a whole year for every rule: exactly one move forward and one back, at the right moments */
    static const struct {
        int rule, y, sm, sd, sh, em, ed, eh;    /* clock reading when each move happens */
    } exp[] = {
        {DST_RULE_UK, 2026, 3, 29, 1, 10, 25, 2},
        {DST_RULE_UK, 2027, 3, 28, 1, 10, 31, 2},
        {DST_RULE_CENTRAL, 2026, 3, 29, 2, 10, 25, 3},
        {DST_RULE_EAST, 2026, 3, 29, 3, 10, 25, 4},
        {DST_RULE_US, 2026, 3, 8, 2, 11, 1, 2},
        {DST_RULE_US, 2027, 3, 14, 2, 11, 7, 2},
    };
    for (unsigned i = 0; i < sizeof(exp) / sizeof(exp[0]); i++) {
        change_t ch[8];
        bool active_end;
        int n = run(exp[i].rule, clk(exp[i].y, 1, 1, 0, 0), false, 365 * 24 * 60 + 600, ch, 8, &active_end);
        CHECK(n == 2);
        CHECK(ch[0].dir == +1 && ch[0].when_mo == exp[i].sm && ch[0].when_d == exp[i].sd &&
              ch[0].when_h == exp[i].sh && ch[0].when_mi == 0);
        CHECK(ch[1].dir == -1 && ch[1].when_mo == exp[i].em && ch[1].when_d == exp[i].ed &&
              ch[1].when_h == exp[i].eh && ch[1].when_mi == 0);
        CHECK(!active_end);
    }

    /* two years in a row, starting in the summer with the right state, and starting with the wrong state */
    {
        change_t ch[8];
        bool active_end;
        int n = run(DST_RULE_UK, clk(2026, 7, 1, 12, 0), true, 2 * 365 * 24 * 60, ch, 8, &active_end);
        CHECK(n == 4);      /* back (Oct 2026), forward and back (2027), forward (2028)... */
        n = run(DST_RULE_UK, clk(2026, 7, 1, 12, 0), false, 60, ch, 8, &active_end);
        CHECK(n == 1 && ch[0].dir == +1 && active_end);         /* told "winter" in summer: corrected at once */
    }

    /* nothing happens for nonsense clock values */
    c = clk(2026, 13, 40, 25, 70);
    CHECK(dst_decide(DST_RULE_UK, &c, false) == 0);
    c = clk(2026, 6, 1, 12, 0);
    CHECK(dst_decide(99, &c, false) == 0);
    CHECK(dst_decide(DST_RULE_UK, &c, false) == +1);      /* summer but the flag says winter */
    CHECK(dst_decide(DST_RULE_UK, &c, true) == 0);

    printf(fails ? "%d FAILED\n" : "all dst tests passed\n", fails);
    return fails != 0;
}
