#pragma once
/* Network time for the unit's clock: a daily check against an NTP server, correcting the unit's clock when it is more
   than a couple of minutes out. The time zone is a standard-time offset from UTC plus a summer time rule (dst.h). */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Start the background task (network time once a day when switched on). */
void autoclock_start(void);

/* ---- network time */

typedef enum {
    AUTOCLOCK_NTP_NEVER,        /* no check since start-up */
    AUTOCLOCK_NTP_CHECKING,
    AUTOCLOCK_NTP_OK,           /* the clock was right */
    AUTOCLOCK_NTP_CORRECTED,    /* the clock was out and has been set */
    AUTOCLOCK_NTP_NO_WIFI,
    AUTOCLOCK_NTP_NO_SYNC,      /* no answer from the time server */
    AUTOCLOCK_NTP_UNIT_OFFLINE, /* the unit's clock could not be read */
} autoclock_ntp_result_t;

#define AUTOCLOCK_TZ_MIN_X10    (-120)  /* standard time offset from UTC, tenths of an hour */
#define AUTOCLOCK_TZ_MAX_X10    140

/* Switch network time on or off, set the standard (winter) time offset from UTC in tenths of an hour, multiples of 5
   (e.g. 0 = UK, 10 = Central Europe, -50 = US Eastern), and the summer time rule (a dst_rule_t, 4 = none). Saved.
   Switching on starts a check soon. */
esp_err_t autoclock_ntp_configure(int on, int16_t tz_std_x10, uint8_t dst_rule);

/* Run a check now (if network time is on). */
void autoclock_ntp_check_now(void);

/* A sentence about the last check, e.g. "Last check 3 h ago: clock was 5 min out, corrected." */
void autoclock_ntp_describe(char *out, size_t size);
