#include "timezones.h"

#include <stdio.h>
#include <stdlib.h>

#include "dst.h"

/* One zone per line, in the form {"name", offset, rule}: tools/web_preview.py reads this table too. Ordered by offset. */
static const tz_zone_t s_zones[] = {
    {"Honolulu", -100, DST_RULE_NONE},
    {"Anchorage", -90, DST_RULE_US},
    {"Los Angeles / Vancouver", -80, DST_RULE_US},
    {"Denver / Calgary", -70, DST_RULE_US},
    {"Phoenix", -70, DST_RULE_NONE},
    {"Chicago / Winnipeg", -60, DST_RULE_US},
    {"Mexico City", -60, DST_RULE_NONE},
    {"New York / Toronto", -50, DST_RULE_US},
    {"Halifax", -40, DST_RULE_US},
    {"Sao Paulo", -30, DST_RULE_NONE},
    {"UTC", 0, DST_RULE_NONE},
    {"Reykjavik", 0, DST_RULE_NONE},
    {"London", 0, DST_RULE_UK},
    {"Dublin", 0, DST_RULE_UK},
    {"Lisbon", 0, DST_RULE_UK},
    {"Lagos", 10, DST_RULE_NONE},
    {"Amsterdam", 10, DST_RULE_CENTRAL},
    {"Berlin", 10, DST_RULE_CENTRAL},
    {"Brussels", 10, DST_RULE_CENTRAL},
    {"Copenhagen", 10, DST_RULE_CENTRAL},
    {"Madrid", 10, DST_RULE_CENTRAL},
    {"Oslo", 10, DST_RULE_CENTRAL},
    {"Paris", 10, DST_RULE_CENTRAL},
    {"Prague", 10, DST_RULE_CENTRAL},
    {"Rome", 10, DST_RULE_CENTRAL},
    {"Stockholm", 10, DST_RULE_CENTRAL},
    {"Vienna", 10, DST_RULE_CENTRAL},
    {"Warsaw", 10, DST_RULE_CENTRAL},
    {"Zurich", 10, DST_RULE_CENTRAL},
    {"Athens", 20, DST_RULE_EAST},
    {"Bucharest", 20, DST_RULE_EAST},
    {"Helsinki", 20, DST_RULE_EAST},
    {"Kyiv", 20, DST_RULE_EAST},
    {"Riga / Tallinn / Vilnius", 20, DST_RULE_EAST},
    {"Sofia", 20, DST_RULE_EAST},
    {"Johannesburg", 20, DST_RULE_NONE},
    {"Istanbul", 30, DST_RULE_NONE},
    {"Moscow", 30, DST_RULE_NONE},
    {"Nairobi", 30, DST_RULE_NONE},
    {"Dubai", 40, DST_RULE_NONE},
    {"Karachi", 50, DST_RULE_NONE},
    {"Delhi / Mumbai", 55, DST_RULE_NONE},
    {"Dhaka", 60, DST_RULE_NONE},
    {"Bangkok / Jakarta", 70, DST_RULE_NONE},
    {"Singapore / Beijing", 80, DST_RULE_NONE},
    {"Perth", 80, DST_RULE_NONE},
    {"Tokyo / Seoul", 90, DST_RULE_NONE},
    {"Brisbane", 100, DST_RULE_NONE},
};

int tz_count(void)
{
    return (int)(sizeof(s_zones) / sizeof(s_zones[0]));
}

const tz_zone_t *tz_get(int index)
{
    return index >= 0 && index < tz_count() ? &s_zones[index] : NULL;
}

int tz_find(int16_t tz_std_x10, uint8_t rule)
{
    for (int i = 0; i < tz_count(); i++) {
        if (s_zones[i].tz_std_x10 == tz_std_x10 && s_zones[i].rule == rule) {
            return i;
        }
    }
    return -1;
}

void tz_format_offset(char *out, int size, int16_t tz_std_x10)
{
    int a = abs(tz_std_x10);
    if (a % 10) {
        snprintf(out, size, "UTC%c%d.%d", tz_std_x10 < 0 ? '-' : '+', a / 10, a % 10);
    } else {
        snprintf(out, size, "UTC%c%d", tz_std_x10 < 0 ? '-' : '+', a / 10);
    }
}
