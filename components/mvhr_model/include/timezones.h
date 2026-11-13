#pragma once
/* A short list of named time zones for network time: city, standard (winter) time offset from UTC and summer time
   rule (dst.h). Only regions whose summer time follows one of the rules in dst.h are listed. */
#include <stdint.h>

typedef struct {
    const char *name;
    int16_t tz_std_x10;     /* tenths of an hour from UTC, standard time */
    uint8_t rule;           /* dst_rule_t, DST_RULE_NONE for no summer time */
} tz_zone_t;

int tz_count(void);
const tz_zone_t *tz_get(int index);

/* Index of the zone with this offset and rule, or -1 (a setting that is not in the list, e.g. from an older version). */
int tz_find(int16_t tz_std_x10, uint8_t rule);

/* "UTC+1", "UTC-5", "UTC+5.5" */
void tz_format_offset(char *out, int size, int16_t tz_std_x10);
