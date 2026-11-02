#include "mvhr_decode.h"

#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* 17 data bytes on panel firmware 1.x units, 20 on the ESPHome author's unit; the first 17 match */
static uint32_t decode_status(mvhr_state_t *st, const uint8_t *d, uint8_t len)
{
    if (len < 17) {
        return 0;
    }
    st->alarms = (uint16_t)(d[0] | (d[1] << 8));
    st->temp_extract_x10 = (int16_t)be16(&d[2]);
    st->temp_supply_x10 = (int16_t)be16(&d[4]);
    st->status = d[6];
    st->humidity_x10 = be16(&d[7]);
    st->fan_speed = (uint8_t)(d[9] <= 2 ? d[9] + 1 : 1);
    st->fan_a_pct = d[10];
    st->fan_b_pct = d[11];
    st->bypass_auto_state = d[12];
    st->frost = d[13] != 0;
    st->bypass_override = d[14] <= 2 ? (int8_t)d[14] : MVHR_BYPASS_AUTO;
    st->bypass_locked = d[15] & 1;
    st->holiday = d[16] != 0;
    return MVHR_G_STATUS;
}

static uint32_t decode_config(mvhr_state_t *st, const uint8_t *d, uint8_t len)
{
    if (len < 3) {
        return 0;
    }
    uint8_t n = len < sizeof(st->cfg) ? len : sizeof(st->cfg);
    memcpy(st->cfg, d, n);
    st->cfg_len = n;
    st->fw_major = d[1];
    st->fw_minor = d[2];
    /* Night inhibition lives at bytes 30..33 in the 49-byte layout (firmware 1.3); other layouts: unknown */
    st->night_known = (d[0] == 0x31 && len >= 34);
    if (st->night_known) {
        uint16_t start = be16(&d[30]);
        st->night_enabled = (start & 0x8000) != 0;
        st->night_start_min = start & 0x07FF;
        st->night_end_min = be16(&d[32]) & 0x07FF;
    }
    return MVHR_G_VERSION;
}

uint32_t mvhr_decode_reply(mvhr_state_t *st, uint8_t cmd, const uint8_t *d, uint8_t len)
{
    switch (cmd) {
    case MVHR_CMD_STATUS:
        return decode_status(st, d, len);
    case MVHR_CMD_FIRE:
        if (len < 1) {
            return 0;
        }
        st->fire_reply = d[0];
        return MVHR_G_FIRE;
    case MVHR_CMD_CLOCK_READ:
        if (len < 9) {
            return 0;
        }
        st->clock = (mvhr_clock_t){
            .hour = d[0], .minute = d[1], .day = d[2], .month = d[3], .year = d[4],
            .format_24h = d[5], .date_format = d[6], .region = d[7], .utc_offset = d[8],
        };
        return MVHR_G_CLOCK;
    case MVHR_CMD_COUNTERS:
        if (len < 9) {
            return 0;
        }
        st->hours = be32(&d[0]);
        st->power_outages = be32(&d[4]);
        st->filter_pct = d[8];
        return MVHR_G_COUNTERS;
    case MVHR_CMD_BOOST_READ:
        if (len < 2) {
            return 0;
        }
        st->boost_delay_s = be16(d);
        return MVHR_G_SETTINGS;
    case MVHR_CMD_OVERRUN_READ:
        if (len < 2) {
            return 0;
        }
        st->overrun_s = be16(d);
        return MVHR_G_SETTINGS;
    case MVHR_CMD_HUMID_READ:
        if (len < 2) {
            return 0;
        }
        st->humidity_setpoint = (uint8_t)be16(d);
        return MVHR_G_SETTINGS;
    case MVHR_CMD_CONFIG_READ:
        return decode_config(st, d, len);
    case MVHR_CMD_VERSION:
        if (len < 2) {
            return 0;
        }
        /* only used when no config block has been read yet */
        if (!(st->valid & MVHR_G_VERSION)) {
            st->fw_major = d[0];
            st->fw_minor = d[1];
        }
        return MVHR_G_VERSION;
    default:
        return 0;   /* write acknowledgements carry no state */
    }
}

void mvhr_encode_clock(const mvhr_clock_t *c, uint8_t out[9])
{
    out[0] = c->hour;
    out[1] = c->minute;
    out[2] = c->day;
    out[3] = c->month;
    out[4] = c->year;
    out[5] = c->format_24h;
    out[6] = c->date_format;
    out[7] = c->region;
    out[8] = c->utc_offset;
}

const char *mvhr_function_text(const mvhr_state_t *st)
{
    switch (st->status & 0x0F) {
    case MVHR_FUNC_SWITCH_DELAY:
        return "Boost starting";
    case MVHR_FUNC_SWITCH_BOOST:
        return "Switch boost";
    case MVHR_FUNC_OVERRUN:
        return "Overrun";
    case MVHR_FUNC_BOOST:
        return st->status & MVHR_IND_HUMIDITY ? "Humidity boost" : st->status & MVHR_IND_CO2 ? "CO2 boost" : "Boost";
    case MVHR_FUNC_BOOST_TIMED:
        return "Timed boost";
    case MVHR_FUNC_PURGE:
        return "Purge";
    case MVHR_FUNC_PURGE_OFF_DELAY:
        return "Purge ending";
    case MVHR_FUNC_NIGHT_INHIBIT:
        return "Night mode";
    default:
        break;
    }
    if (st->holiday) {
        return "Holiday";
    }
    if (st->frost) {
        return "Frost protection";
    }
    return NULL;
}

/* Boost as one word: light-switch/humidity/timed boosts and purge count as on, their run-down as overrun */
const char *mvhr_boost_text(const mvhr_state_t *st)
{
    switch (st->status & 0x0F) {
    case MVHR_FUNC_SWITCH_DELAY:
        return "Delay";
    case MVHR_FUNC_SWITCH_BOOST:
    case MVHR_FUNC_BOOST:
    case MVHR_FUNC_BOOST_TIMED:
    case MVHR_FUNC_PURGE:
        return "On";
    case MVHR_FUNC_OVERRUN:
    case MVHR_FUNC_PURGE_OFF_DELAY:
        return "Overrun";
    default:
        return "Off";
    }
}

/* What the air does now: the override if one is set, otherwise the unit's own choice */
const char *mvhr_airflow_text(const mvhr_state_t *st)
{
    static const char *const names[] = {"Heat recovery", "Bypass", "Intake motor off"};
    int mode = st->bypass_override >= 0 && st->bypass_override <= 2 ? st->bypass_override : st->bypass_auto_state;
    return mode <= 2 ? names[mode] : "Heat recovery";
}

/* most important first, same order as the panel's alarm sequencer */
const char *mvhr_alarm_text(uint16_t alarms)
{
    static const struct {
        uint16_t bit;
        const char *text;
    } texts[] = {
        {MVHR_ALARM_FIRE, "Fire alarm"},
        {MVHR_ALARM_DRAIN_PAN_FULL, "Condensate drain pan full"},
        {MVHR_ALARM_INTAKE_FAN_1, "Intake fan fault (MTR-1)"},
        {MVHR_ALARM_EXHAUST_FAN_1, "Exhaust fan fault (MTR-1)"},
        {MVHR_ALARM_INTAKE_FAN_2, "Intake fan fault (MTR-2)"},
        {MVHR_ALARM_EXHAUST_FAN_2, "Exhaust fan fault (MTR-2)"},
        {MVHR_ALARM_NTC1, "Temperature probe fault (NTC1)"},
        {MVHR_ALARM_NTC2, "Temperature probe fault (NTC2)"},
        {MVHR_ALARM_RTC, "Real-time clock defective"},
        {MVHR_ALARM_CLOCK_BATTERY, "Clock battery is low"},
        {MVHR_ALARM_CLOCK_INACCURATE, "Clock needs setting"},
        {MVHR_ALARM_CLEAN_FILTERS, "Clean the filters"},
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (alarms & texts[i].bit) {
            return texts[i].text;
        }
    }
    return alarms ? "Unknown alarm" : "None";
}
