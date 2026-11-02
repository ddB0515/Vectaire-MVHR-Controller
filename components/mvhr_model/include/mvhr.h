#pragma once
/*
 * MVHR unit model. Owns the bus: one task polls the unit, decodes replies into mvhr_state_t and sends
 * writes requested by the UI or Home Assistant. Commands and fields were reverse-engineered from the original panel firmware.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Alarm bits (status bytes 0-1) */
#define MVHR_ALARM_FIRE             (1u << 0)
#define MVHR_ALARM_DRAIN_PAN_FULL   (1u << 1)
#define MVHR_ALARM_NTC1             (1u << 2)
#define MVHR_ALARM_NTC2             (1u << 3)
#define MVHR_ALARM_CLOCK_BATTERY    (1u << 4)
#define MVHR_ALARM_CLOCK_INACCURATE (1u << 5)
#define MVHR_ALARM_RTC              (1u << 6)
#define MVHR_ALARM_CLEAN_FILTERS    (1u << 7)
#define MVHR_ALARM_INTAKE_FAN_1     (1u << 8)
#define MVHR_ALARM_EXHAUST_FAN_2    (1u << 9)
#define MVHR_ALARM_INTAKE_FAN_2     (1u << 10)
#define MVHR_ALARM_EXHAUST_FAN_1    (1u << 11)

/* Active function, low nibble of the status byte */
typedef enum {
    MVHR_FUNC_NONE = 0,
    MVHR_FUNC_SWITCH_DELAY = 1,     /* light-switch boost, activation delay running */
    MVHR_FUNC_SWITCH_BOOST = 2,     /* boost via light switch */
    MVHR_FUNC_OVERRUN = 3,          /* overrun after switch off */
    MVHR_FUNC_BOOST = 4,
    MVHR_FUNC_BOOST_TIMED = 5,
    MVHR_FUNC_PURGE = 6,
    MVHR_FUNC_PURGE_OFF_DELAY = 7,
    MVHR_FUNC_NIGHT_INHIBIT = 8,
} mvhr_func_t;

/* Indicator bits, high nibble of the status byte */
#define MVHR_IND_HUMIDITY   0x10
#define MVHR_IND_CO2        0x20
#define MVHR_IND_SETTINGS   0x40
#define MVHR_IND_HEATER     0x80

#define MVHR_BYPASS_AUTO    (-1)

typedef struct {
    uint8_t hour, minute, day, month, year;     /* year = 2000 + year */
    uint8_t format_24h;                         /* 1 = 24 h */
    uint8_t date_format;                        /* 0 MM/DD/YYYY, 1 DD/MM/YYYY, 2 YYYY/MM/DD */
    uint8_t region;                             /* index into the unit's region list */
    uint8_t utc_offset;                         /* raw, bit 7 = DST flag */
} mvhr_clock_t;

/* Validity bits for mvhr_state_t groups, also used as change mask in notifications */
#define MVHR_G_STATUS       (1u << 0)
#define MVHR_G_CLOCK        (1u << 1)
#define MVHR_G_COUNTERS     (1u << 2)
#define MVHR_G_SETTINGS     (1u << 3)   /* boost delay, overrun, humidity setpoint */
#define MVHR_G_VERSION      (1u << 4)   /* firmware version, config block, night inhibition */
#define MVHR_G_ONLINE       (1u << 5)
#define MVHR_G_FIRE         (1u << 6)

typedef struct {
    uint32_t valid;             /* MVHR_G_* that have been received at least once */
    bool online;

    /* status (cmd 0x00) */
    uint16_t alarms;
    int16_t temp_extract_x10;   /* inside / extract air, 0.1 C */
    int16_t temp_supply_x10;    /* outside / supply air, 0.1 C */
    uint8_t status;             /* raw status byte: function (low nibble) + indicators */
    uint16_t humidity_x10;      /* status bytes 7-8: relative humidity (inside), 0.1 %; see mvhr_humidity_shown() */
    uint8_t fan_speed;          /* 1..3 */
    uint8_t fan_a_pct;          /* status byte 10: probably intake fan duty, 0..100 % */
    uint8_t fan_b_pct;          /* status byte 11: probably exhaust fan duty, 0..100 % */
    uint8_t bypass_auto_state;  /* 0 heat recovery, 1 bypass, 2 third state */
    bool frost;
    int8_t bypass_override;     /* MVHR_BYPASS_AUTO or 0..2 */
    bool bypass_locked;
    bool holiday;

    uint8_t fire_reply;         /* cmd 0x01 reply byte (bit1 = still active) */

    mvhr_clock_t clock;         /* cmd 0x02 */

    uint32_t hours;             /* cmd 0x06 */
    uint32_t power_outages;
    uint8_t filter_pct;

    uint16_t boost_delay_s;     /* cmd 0x08 */
    uint16_t overrun_s;         /* cmd 0x0A */
    uint8_t humidity_setpoint;  /* cmd 0x0C */

    uint8_t fw_major, fw_minor; /* cmd 0x16 */
    uint8_t cfg_len;            /* cmd 0x0F block (first data byte = its length) */
    uint8_t cfg[80];
    bool night_known;           /* night inhibition decodable from this config layout */
    bool night_enabled;
    uint16_t night_start_min, night_end_min;
} mvhr_state_t;

typedef void (*mvhr_listener_t)(uint32_t changed, void *arg);

/* simulated = talk to the built-in simulated unit instead of the bus (see mvhr_sim.h) */
esp_err_t mvhr_start(bool simulated);
bool mvhr_is_simulated(void);

/* Snapshot of the current state (thread-safe). */
void mvhr_get_state(mvhr_state_t *out);

/* Called from the model task after each change; keep it short (post to your own queue). */
esp_err_t mvhr_add_listener(mvhr_listener_t cb, void *arg);

/* Commands: queued and executed by the model task. */
esp_err_t mvhr_set_fan_speed(uint8_t speed_1_3);
esp_err_t mvhr_set_bypass(int8_t mode);             /* MVHR_BYPASS_AUTO or 0..2 */
esp_err_t mvhr_set_holiday(bool on);
esp_err_t mvhr_set_boost_delay(uint16_t seconds);   /* 0..300 */
esp_err_t mvhr_set_overrun(uint16_t seconds);       /* 0..5400 */
esp_err_t mvhr_set_humidity_setpoint(uint8_t pct);  /* 35..99 */
esp_err_t mvhr_set_night_inhibition(bool enabled, uint16_t start_min, uint16_t end_min);
esp_err_t mvhr_set_time(uint8_t hour, uint8_t minute);
esp_err_t mvhr_set_date(uint8_t day, uint8_t month, uint16_t year);
esp_err_t mvhr_set_clock(uint8_t hour, uint8_t minute, uint8_t day, uint8_t month, uint16_t year);  /* both at once */
esp_err_t mvhr_reset_filter(void);
esp_err_t mvhr_fire_alarm_resume(void);
void mvhr_refresh(uint32_t groups);                 /* poll these groups as soon as possible */

/* The original panel shows the humidity only within 35..99 %; outside that it is "low" or "high". */
#define MVHR_RH_SHOWN_MIN_X10   350
#define MVHR_RH_SHOWN_MAX_X10   990
#define MVHR_RH_VALID_MAX_X10   1000    /* above: no reading */

/* Short English texts (pure, used by the UI and Home Assistant) */
const char *mvhr_function_text(const mvhr_state_t *st);   /* active function, holiday, frost; NULL if none */
const char *mvhr_alarm_text(uint16_t alarms);             /* most important alarm, "None" if none */
const char *mvhr_boost_text(const mvhr_state_t *st);      /* "Off", "Delay", "On" or "Overrun" */
const char *mvhr_airflow_text(const mvhr_state_t *st);    /* "Heat recovery", "Bypass" or "Intake motor off" */

#ifdef __cplusplus
}
#endif
