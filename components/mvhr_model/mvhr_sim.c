#include "mvhr_sim.h"

#include <stdbool.h>
#include <string.h>

#include "mvhr_decode.h"

#define CFG_LEN         49
#define FILTER_STEP_MS  (10 * 60 * 1000)    /* filter saturation +1 % every 10 min */

static struct {
    uint16_t alarms;
    uint8_t fan;                /* 0..2 */
    uint8_t bypass_override;    /* 0..2, 0xFF = auto */
    uint8_t holiday;
    uint16_t boost_delay_s, overrun_s, humidity_setpoint;
    uint8_t cfg[CFG_LEN];
    uint8_t clock[9];           /* clock block as last set ... */
    uint32_t clock_ms;          /* ... at this time */
    uint8_t filter_pct;         /* saturation at filter_ms */
    uint32_t filter_ms;
} s;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v & 0xFFFF);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Same layout as default_block() in unit_sim.py */
static void default_config(uint8_t *b)
{
    memset(b, 0, CFG_LEN);
    b[0] = CFG_LEN;
    b[1] = 1;                                   /* firmware 1.3 */
    b[2] = 3;
    static const uint8_t speed_pct[3] = {30, 50, 80};
    for (int i = 0; i < 3; i++) {
        put16(&b[8 + 2 * i], speed_pct[i] * 4096 / 100);   /* intake */
        put16(&b[14 + 2 * i], speed_pct[i] * 4096 / 100);  /* exhaust */
    }
    put16(&b[20], 20 * 4096 / 100);             /* stall speed */
    put16(&b[22], (uint16_t)-30);               /* frost reduction -3.0 C */
    put16(&b[24], (uint16_t)-100);              /* frost stop -10.0 C */
    put16(&b[26], 240);                         /* bypass upper 24.0 C */
    put16(&b[28], 160);                         /* bypass lower 16.0 C */
    put16(&b[30], 0x8000 | 22 * 60);            /* night inhibition 22:00 .. */
    put16(&b[32], 6 * 60);                      /* .. 06:00, enabled */
    b[36] = 1;
    b[38] = 70;                                 /* humidity threshold */
    put16(&b[41], 50);                          /* heater threshold 5.0 C */
    b[45] = 30;                                 /* purge off-delay, min */
    put16(&b[47], 500);                         /* holiday reduction 50.0 % */
}

void mvhr_sim_init(void)
{
    memset(&s, 0, sizeof(s));
    s.fan = 1;
    s.bypass_override = 0xFF;
    s.boost_delay_s = 60;
    s.overrun_s = 900;
    s.humidity_setpoint = 65;
    default_config(s.cfg);
    static const uint8_t clock[9] = {12, 0, 28, 10, 26, 1, 1, 0, 0};    /* 12:00 28/10/2026, 24 h, DD/MM */
    memcpy(s.clock, clock, sizeof(clock));
    s.filter_pct = 63;
}

/* Triangle wave between -amp and +amp */
static int tri(uint32_t t_s, uint32_t period_s, int amp)
{
    uint32_t p = t_s % period_s;
    int32_t x = (int32_t)(p < period_s / 2 ? p : period_s - p);
    return (int)(x * 4 * amp / (int32_t)period_s) - amp;
}

static uint8_t days_in_month(uint8_t month, uint8_t year)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1] + (month == 2 && year % 4 == 0);
}

static void clock_now(uint8_t c[9], uint32_t now_ms)
{
    memcpy(c, s.clock, 9);
    uint32_t mins = c[0] * 60 + c[1] + (now_ms - s.clock_ms) / 60000;
    c[0] = mins / 60 % 24;
    c[1] = mins % 60;
    for (uint32_t days = mins / 1440; days; days--) {
        if (++c[2] > days_in_month(c[3], c[4])) {
            c[2] = 1;
            if (++c[3] > 12) {
                c[3] = 1;
                c[4]++;
            }
        }
    }
}

static uint8_t filter_now(uint32_t now_ms)
{
    uint32_t pct = s.filter_pct + (now_ms - s.filter_ms) / FILTER_STEP_MS;
    return pct > 100 ? 100 : (uint8_t)pct;
}

static bool night_active(uint32_t now_ms)
{
    uint16_t start = get16(&s.cfg[30]), end = get16(&s.cfg[32]) & 0x07FF;
    if (!(start & 0x8000)) {
        return false;
    }
    start &= 0x07FF;
    uint8_t c[9];
    clock_now(c, now_ms);
    uint16_t m = c[0] * 60 + c[1];
    return start <= end ? (m >= start && m < end) : (m >= start || m < end);
}

static int status(uint8_t *out, uint32_t now_ms)
{
    uint32_t t = now_ms / 1000;
    int16_t extract = 210 + tri(t, 600, 8);         /* 20.2 .. 21.8 C over 10 min */
    int16_t supply = 85 + tri(t, 1800, 40);         /* 4.5 .. 12.5 C over 30 min */
    uint8_t rh_extract = 55 + tri(t, 420, 22);      /* 33 .. 77 %, crosses the 65 % setpoint and goes "low" */

    uint8_t st = MVHR_FUNC_NONE;
    if (rh_extract > s.humidity_setpoint) {
        st = MVHR_FUNC_BOOST | MVHR_IND_HUMIDITY;
    }
    if (night_active(now_ms)) {
        st = (st & 0xF0) | MVHR_FUNC_NIGHT_INHIBIT;
    }
    uint16_t alarms = s.alarms | (filter_now(now_ms) >= 100 ? MVHR_ALARM_CLEAN_FILTERS : 0);

    out[0] = alarms & 0xFF;
    out[1] = alarms >> 8;
    put16(&out[2], (uint16_t)extract);
    put16(&out[4], (uint16_t)supply);
    out[6] = st;
    put16(&out[7], rh_extract * 10);
    out[9] = s.fan;
    /* fan duties: higher with each speed and during boost, lower on holiday */
    uint8_t duty = (st & 0x0F) == MVHR_FUNC_BOOST ? 100 : 30 + 20 * (s.fan + 1);   /* s.fan is 0..2 */
    duty = s.holiday ? duty / 2 : duty;
    out[10] = duty;
    out[11] = duty - 10;
    out[12] = s.bypass_override <= 2 ? s.bypass_override : 0;
    out[13] = supply < -30;
    out[14] = s.bypass_override;
    out[15] = 0;
    out[16] = s.holiday;
    return 17;
}

int mvhr_sim_reply(uint8_t cmd, const uint8_t *d, uint8_t len, uint8_t *out, uint32_t now_ms)
{
    switch (cmd) {
    case MVHR_CMD_STATUS:
        return status(out, now_ms);
    case MVHR_CMD_FIRE:
        if (len < 1) {
            return -1;
        }
        if (d[0] == 1) {
            s.alarms &= ~MVHR_ALARM_FIRE;
        }
        out[0] = s.alarms & MVHR_ALARM_FIRE ? 2 : 0;
        out[1] = 0;
        return 2;
    case MVHR_CMD_CLOCK_READ:
        clock_now(out, now_ms);
        return 9;
    case MVHR_CMD_CLOCK_TIME:
    case MVHR_CMD_CLOCK_DATE:
    case MVHR_CMD_CLOCK_REGION:
        if (len < 9) {
            return -1;
        }
        memcpy(s.clock, d, 9);
        s.clock_ms = now_ms;
        out[0] = 1;
        return 1;
    case MVHR_CMD_COUNTERS:
        put32(&out[0], 12345 + now_ms / 3600000);
        put32(&out[4], 7);
        out[8] = filter_now(now_ms);
        return 9;
    case MVHR_CMD_FILTER_RESET:
        s.filter_pct = 0;
        s.filter_ms = now_ms;
        return 0;
    case MVHR_CMD_BOOST_READ:
        put16(out, s.boost_delay_s);
        return 2;
    case MVHR_CMD_OVERRUN_READ:
        put16(out, s.overrun_s);
        return 2;
    case MVHR_CMD_HUMID_READ:
        put16(out, s.humidity_setpoint);
        return 2;
    case MVHR_CMD_BOOST_WRITE:
        if (len < 2) {
            return -1;
        }
        s.boost_delay_s = get16(d);
        return 0;
    case MVHR_CMD_OVERRUN_WRITE:
        if (len < 2) {
            return -1;
        }
        s.overrun_s = get16(d);
        return 0;
    case MVHR_CMD_HUMID_WRITE:
        if (len < 2) {
            return -1;
        }
        s.humidity_setpoint = get16(d);
        return 0;
    case MVHR_CMD_FAN:
        if (len < 1 || d[0] > 2) {
            return -1;
        }
        s.fan = d[0];
        return 0;
    case MVHR_CMD_CONFIG_READ:
        memcpy(out, s.cfg, CFG_LEN);
        return CFG_LEN;
    case MVHR_CMD_CONFIG_WRITE:
        if (len < CFG_LEN) {
            return -1;
        }
        memcpy(s.cfg, d, CFG_LEN);
        return 0;
    case MVHR_CMD_BYPASS:
        if (len < 1) {
            return -1;
        }
        s.bypass_override = d[0];
        return 0;
    case MVHR_CMD_NIGHT:
        if (len < 4) {
            return -1;
        }
        memcpy(&s.cfg[30], d, 4);
        return 0;
    case MVHR_CMD_HOLIDAY:
        if (len < 1) {
            return -1;
        }
        s.holiday = d[0] != 0;
        return 0;
    case MVHR_CMD_VERSION:
        out[0] = 1;
        out[1] = 3;
        return 2;
    default:
        /* includes 0x13 fan test and the 0xB0-0xB2 bootloader frames: never acknowledged */
        return -1;
    }
}
