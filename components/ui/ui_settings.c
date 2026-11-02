/* Menu and the screens behind it: bypass, holiday, night mode, humidity, timers, clock, filter, device. */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "ha.h"
#include "net.h"
#include "settings.h"
#include "ui_priv.h"

#define PICK_PENDING_MS 4000

/* ---------------------------------------------------------------- shared pieces */

typedef struct {
    lv_obj_t *row, *title, *sub, *value;
} row_t;

/* A list row: optional icon, title, optional subtitle, optional value button on the right. */
static void row_create(row_t *r, lv_obj_t *scr, int y, int h, const lv_image_dsc_t *icon, const char *title,
                       const char *sub, const char *value, lv_event_cb_t value_cb, void *arg)
{
    memset(r, 0, sizeof(*r));
    r->row = ui_box(scr);
    lv_obj_set_size(r->row, SCR_W - 20, h);
    lv_obj_set_pos(r->row, 10, y);
    lv_obj_set_style_border_side(r->row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r->row, 1, 0);
    lv_obj_set_style_border_color(r->row, COL_LINE, 0);
    int x = 0;
    if (icon) {
        lv_obj_align(ui_icon(r->row, icon, COL_ACCENT), LV_ALIGN_LEFT_MID, 0, 0);
        x = 30;
    }
    lv_obj_t *col = ui_box(r->row);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, x, 0);
    r->title = ui_label(col, &lv_font_montserrat_14, COL_TEXT, title);
    if (sub) {
        r->sub = ui_label(col, &lv_font_montserrat_12, COL_TEXT2, sub);
    }
    if (value_cb) {
        lv_obj_t *b = ui_button(r->row, value ? value : "", false, value_cb, arg);
        lv_obj_set_size(b, 92, 36);
        lv_obj_set_style_bg_color(b, COL_PANEL, 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        r->value = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_font(r->value, &lv_font_montserrat_14, 0);
    }
}

static lv_obj_t *themed_switch(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 56, 30);
    lv_obj_set_style_bg_color(sw, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, COL_WHITE, LV_PART_KNOB);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* Radio-style option rows (bypass, holiday) */
typedef struct {
    const lv_image_dsc_t *icon;
    lv_color_t icon_col;
    const char *title, *sub;
} option_t;

static lv_obj_t *s_opt_btn[3], *s_opt_dot[3], *s_opt_sub[3];
static int s_opt_n, s_pick = -1;
static uint32_t s_pick_ms;

static void options_create(lv_obj_t *scr, const option_t *opts, int n, lv_event_cb_t cb)
{
    s_opt_n = n;
    s_pick = -1;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(scr);
        ui_style_button(b, false);
        lv_obj_set_size(b, SCR_W - 16, 50);
        lv_obj_set_pos(b, 8, TITLE_H + 8 + i * 56);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_align(ui_icon(b, opts[i].icon, opts[i].icon_col), LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *col = ui_box(b);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 1, 0);
        lv_obj_align(col, LV_ALIGN_LEFT_MID, 46, 0);
        ui_label(col, &lv_font_montserrat_14, COL_TEXT, opts[i].title);
        s_opt_sub[i] = ui_label(col, &lv_font_montserrat_12, COL_TEXT2, opts[i].sub);
        lv_obj_t *ring = ui_box(b);
        lv_obj_set_size(ring, 18, 18);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, COL_HOUSE, 0);
        lv_obj_align(ring, LV_ALIGN_RIGHT_MID, -10, 0);
        s_opt_dot[i] = ui_box(ring);
        lv_obj_set_size(s_opt_dot[i], 8, 8);
        lv_obj_set_style_radius(s_opt_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_opt_dot[i], COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(s_opt_dot[i], LV_OPA_COVER, 0);
        lv_obj_center(s_opt_dot[i]);
        s_opt_btn[i] = b;
    }
}

/* Highlight the option the unit reports, or the one just picked until the unit confirms it. */
static void options_select(int actual, bool enabled)
{
    int sel = actual;
    if (s_pick >= 0 && lv_tick_elaps(s_pick_ms) < PICK_PENDING_MS && s_pick != actual) {
        sel = s_pick;
    } else {
        s_pick = -1;
    }
    for (int i = 0; i < s_opt_n; i++) {
        bool on = i == sel;
        lv_obj_set_style_border_width(s_opt_btn[i], on ? 2 : 1, 0);
        lv_obj_set_style_border_color(s_opt_btn[i], on ? COL_ACCENT : COL_BORDER, 0);
        lv_obj_set_style_bg_color(s_opt_btn[i], on ? COL_ACCENT_BG : COL_BG, 0);
        lv_obj_set_flag(s_opt_dot[i], LV_OBJ_FLAG_HIDDEN, !on);
        lv_obj_set_style_border_color(lv_obj_get_parent(s_opt_dot[i]), on ? COL_ACCENT : COL_HOUSE, 0);
        lv_obj_set_state(s_opt_btn[i], LV_STATE_DISABLED, !enabled);
    }
}

static void options_picked(int i)
{
    s_pick = i;
    s_pick_ms = lv_tick_get();
}

static bool unit_ok(const mvhr_state_t *st)
{
    return st->online && (st->valid & MVHR_G_STATUS);
}

/* ---------------------------------------------------------------- menu */

static lv_obj_t *s_filter_dot;

static void menu_refresh(const mvhr_state_t *st)
{
    lv_obj_set_flag(s_filter_dot, LV_OBJ_FLAG_HIDDEN, !(st->alarms & MVHR_ALARM_CLEAN_FILTERS));
}

void ui_show_menu(void)
{
    static const struct {
        const lv_image_dsc_t *icon;
        const char *text;
        ui_back_fn go;
    } tiles[] = {
        {&ic_holiday_28, "Holiday", ui_show_holiday}, {&ic_moon_28, "Night", ui_show_night},
        {&ic_droplet_28, "Humidity", ui_show_humidity}, {&ic_hourglass_28, "Timers", ui_show_timers},
        {&ic_exchange_28, "Bypass", ui_show_bypass}, {&ic_clock_28, "Clock", ui_show_clock},
        {&ic_filter_28, "Filter", ui_show_counters}, {&ic_device_28, "Device", ui_show_device},
    };
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Menu", ui_show_home);
    lv_obj_t *grid = ui_box(scr);
    lv_obj_set_size(grid, SCR_W, SCR_H - TITLE_H);
    lv_obj_set_pos(grid, 0, TITLE_H);
    lv_obj_set_style_pad_all(grid, 8, 0);
    lv_obj_set_style_pad_gap(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        lv_obj_t *b = lv_button_create(grid);
        ui_style_button(b, false);
        lv_obj_set_style_bg_color(b, COL_TILE, 0);
        lv_obj_set_style_border_color(b, COL_LINE, 0);
        lv_obj_set_size(b, 71, 91);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 8, 0);
        lv_obj_add_event_cb(b, ui_go_cb, LV_EVENT_CLICKED, (void *)tiles[i].go);
        ui_icon(b, tiles[i].icon, COL_ACCENT);
        ui_label(b, &lv_font_montserrat_12, COL_TEXT, tiles[i].text);
        if (tiles[i].go == ui_show_counters) {
            s_filter_dot = ui_box(b);
            lv_obj_add_flag(s_filter_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_size(s_filter_dot, 10, 10);
            lv_obj_set_style_radius(s_filter_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_filter_dot, COL_HOT, 0);
            lv_obj_set_style_bg_opa(s_filter_dot, LV_OPA_COVER, 0);
            lv_obj_align(s_filter_dot, LV_ALIGN_TOP_RIGHT, -6, 6);
        }
    }
    ui_load(scr, SCR_MENU, menu_refresh);
}

/* ---------------------------------------------------------------- bypass */

static void bypass_refresh(const mvhr_state_t *st)
{
    static const char *now[] = {"Now: heat recovery", "Now: bypass", "Now: intake motor off"};
    lv_label_set_text(s_opt_sub[0], unit_ok(st) && st->bypass_auto_state <= 2 ? now[st->bypass_auto_state]
                                                                                : "Unit decides by temperature");
    int actual = st->bypass_override == MVHR_BYPASS_AUTO ? 0 : st->bypass_override == 0 ? 1
                 : st->bypass_override == 1 ? 2 : -1;
    options_select(actual, unit_ok(st));
}

static void bypass_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const int8_t mode[] = {MVHR_BYPASS_AUTO, 0, 1};
    mvhr_set_bypass(mode[i]);
    options_picked(i);
    mvhr_state_t st;
    mvhr_get_state(&st);
    bypass_refresh(&st);
}

void ui_show_bypass(void)
{
    const option_t opts[] = {
        {&ic_exchange_24, COL_ACCENT, "Automatic", ""},
        {&ic_flame_24, COL_HOT, "Heat recovery", "Keep the heat inside (winter)"},
        {&ic_wind_24, COL_ACCENT, "Bypass", "Fresh air, no heat exchange"},
    };
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Bypass", ui_show_menu);
    options_create(scr, opts, 3, bypass_pick);
    ui_load(scr, SCR_SUB, bypass_refresh);
}

/* ---------------------------------------------------------------- holiday */

static void holiday_refresh(const mvhr_state_t *st)
{
    options_select(st->holiday ? 1 : 0, unit_ok(st));
}

static void holiday_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mvhr_set_holiday(i == 1);
    options_picked(i);
    mvhr_state_t st;
    mvhr_get_state(&st);
    holiday_refresh(&st);
}

void ui_show_holiday(void)
{
    const option_t opts[] = {
        {&ic_home_24, COL_ACCENT, "Off", "Normal ventilation"},
        {&ic_holiday_24, COL_ACCENT, "On", "Reduced ventilation while away"},
    };
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Holiday", ui_show_menu);
    options_create(scr, opts, 2, holiday_pick);
    ui_load(scr, SCR_SUB, holiday_refresh);
}

/* ---------------------------------------------------------------- humidity */

static void set_humidity(int v)
{
    mvhr_set_humidity_setpoint((uint8_t)v);
}

void ui_show_humidity(void)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    int v = (st.valid & MVHR_G_SETTINGS) && st.humidity_setpoint >= 35 ? st.humidity_setpoint : 65;
    ui_edit_value("Humidity", "Boost when inside humidity is above", v, 35, 99, 1, "%d %%", set_humidity,
                  ui_show_menu);
}

/* ---------------------------------------------------------------- timers */

static row_t s_row_a, s_row_b, s_row_c;

static void timers_refresh(const mvhr_state_t *st)
{
    bool ok = st->valid & MVHR_G_SETTINGS;
    lv_label_set_text_fmt(s_row_a.value, ok ? "%u s" : "-", st->boost_delay_s);
    lv_label_set_text_fmt(s_row_b.value, ok ? "%u min" : "-", st->overrun_s / 60);
}

static void set_boost_delay(int v)
{
    mvhr_set_boost_delay((uint16_t)v);
}

static void set_overrun(int v)
{
    mvhr_set_overrun((uint16_t)(v * 60));
}

static void edit_boost_delay(lv_event_t *e)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    ui_edit_value("Boost delay", "Wait after the light switch before boosting", st.boost_delay_s, 0, 300, 5, "%d s",
                  set_boost_delay, ui_show_timers);
}

static void edit_overrun(lv_event_t *e)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    ui_edit_value("Overrun", "Keep boosting after the light switch is off", st.overrun_s / 60, 0, 90, 1, "%d min",
                  set_overrun, ui_show_timers);
}

void ui_show_timers(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Timers", ui_show_menu);
    row_create(&s_row_a, scr, TITLE_H + 6, 56, &ic_hourglass_20, "Boost delay", "Before a switch boost starts", "-",
               edit_boost_delay, NULL);
    row_create(&s_row_b, scr, TITLE_H + 62, 56, &ic_clock_20, "Overrun", "After the switch is off", "-",
               edit_overrun, NULL);
    ui_load(scr, SCR_SUB, timers_refresh);
}

/* ---------------------------------------------------------------- night mode (the unit's night inhibition) */

static lv_obj_t *s_night_switch, *s_night_note;
static bool s_night_on;
static uint16_t s_night_start, s_night_end;

static void night_refresh(const mvhr_state_t *st)
{
    bool known = st->night_known;
    lv_obj_set_flag(s_night_note, LV_OBJ_FLAG_HIDDEN, known);
    if (!known) {
        return;
    }
    s_night_on = st->night_enabled;
    s_night_start = st->night_start_min;
    s_night_end = st->night_end_min;
    lv_obj_set_state(s_night_switch, LV_STATE_CHECKED, s_night_on);
    lv_label_set_text_fmt(s_row_b.value, "%02u:%02u", s_night_start / 60, s_night_start % 60);
    lv_label_set_text_fmt(s_row_c.value, "%02u:%02u", s_night_end / 60, s_night_end % 60);
}

static void night_switch_cb(lv_event_t *e)
{
    s_night_on = lv_obj_has_state(s_night_switch, LV_STATE_CHECKED);
    mvhr_set_night_inhibition(s_night_on, s_night_start, s_night_end);
}

static bool s_edit_start;

static void night_time_set(const int *v)
{
    uint16_t m = (uint16_t)(v[0] * 60 + v[1]);
    mvhr_set_night_inhibition(s_night_on, s_edit_start ? m : s_night_start, s_edit_start ? s_night_end : m);
}

static void night_edit(lv_event_t *e)
{
    s_edit_start = lv_event_get_user_data(e) != NULL;
    uint16_t m = s_edit_start ? s_night_start : s_night_end;
    const ui_field_t f[] = {{m / 60, 0, 23, 1, "%02d"}, {m % 60, 0, 59, 5, "%02d"}};
    ui_edit_fields(s_edit_start ? "Night starts" : "Night ends", f, 2, ":", night_time_set, ui_show_night);
}

void ui_show_night(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Night mode", ui_show_menu);
    row_create(&s_row_a, scr, TITLE_H + 4, 56, &ic_moon_20, "No boost at night", "Quiet between these times", NULL,
               NULL, NULL);
    s_night_switch = themed_switch(s_row_a.row, night_switch_cb);
    row_create(&s_row_b, scr, TITLE_H + 60, 50, NULL, "Start", NULL, "--:--", night_edit, (void *)1);
    row_create(&s_row_c, scr, TITLE_H + 110, 50, NULL, "End", NULL, "--:--", night_edit, NULL);
    s_night_note = ui_label(scr, &lv_font_montserrat_12, COL_WARN_TEXT,
                            "Not available: the unit's settings\nhaven't been read yet or use an unknown layout.");
    lv_obj_set_style_bg_color(s_night_note, COL_WARN_BG, 0);
    lv_obj_set_style_bg_opa(s_night_note, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_night_note, 8, 0);
    lv_obj_set_width(s_night_note, SCR_W - 20);
    lv_obj_align(s_night_note, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_load(scr, SCR_SUB, night_refresh);
}

/* ---------------------------------------------------------------- clock */

static lv_obj_t *s_clock_time, *s_clock_date, *s_clock_note;

static void clock_refresh(const mvhr_state_t *st)
{
    if (st->valid & MVHR_G_CLOCK) {
        lv_label_set_text_fmt(s_clock_time, "%02u:%02u", st->clock.hour, st->clock.minute);
        lv_label_set_text_fmt(s_clock_date, "%02u / %02u / %u", st->clock.day, st->clock.month,
                              2000 + st->clock.year);
    }
    lv_obj_set_flag(s_clock_note, LV_OBJ_FLAG_HIDDEN,
                    !(st->alarms & (MVHR_ALARM_CLOCK_INACCURATE | MVHR_ALARM_CLOCK_BATTERY | MVHR_ALARM_RTC)));
}

static void time_set(const int *v)
{
    mvhr_set_time((uint8_t)v[0], (uint8_t)v[1]);
}

static void date_set(const int *v)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max = days[v[1] - 1] + (v[1] == 2 && v[2] % 4 == 0);
    mvhr_set_date((uint8_t)(v[0] > max ? max : v[0]), (uint8_t)v[1], (uint16_t)v[2]);
}

static void set_time_cb(lv_event_t *e)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    const ui_field_t f[] = {{st.clock.hour, 0, 23, 1, "%02d"}, {st.clock.minute, 0, 59, 1, "%02d"}};
    ui_edit_fields("Set time", f, 2, ":", time_set, ui_show_clock);
}

static void set_date_cb(lv_event_t *e)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    const ui_field_t f[] = {{st.clock.day ? st.clock.day : 1, 1, 31, 1, "%02d"},
                            {st.clock.month ? st.clock.month : 1, 1, 12, 1, "%02d"},
                            {2000 + st.clock.year, 2024, 2099, 1, "%d"}};
    ui_edit_fields("Set date", f, 3, "/", date_set, ui_show_clock);
}

void ui_show_clock(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Clock", ui_show_menu);
    s_clock_time = ui_label(scr, &lv_font_montserrat_32, COL_TEXT, "--:--");
    lv_obj_align(s_clock_time, LV_ALIGN_TOP_MID, 0, TITLE_H + 22);
    s_clock_date = ui_label(scr, &lv_font_montserrat_16, COL_TEXT2, "");
    lv_obj_align(s_clock_date, LV_ALIGN_TOP_MID, 0, TITLE_H + 64);
    s_clock_note = ui_label(scr, &lv_font_montserrat_12, COL_WARN_TEXT, "The unit's clock needs attention.");
    lv_obj_align(s_clock_note, LV_ALIGN_TOP_MID, 0, TITLE_H + 96);
    lv_obj_t *row = ui_bottom_buttons(scr, "Set time", set_time_cb, "Set date", set_date_cb, COL_ACCENT);
    lv_obj_t *left = lv_obj_get_child(row, 0);
    lv_obj_set_style_bg_color(left, COL_BG, 0);
    ui_load(scr, SCR_SUB, clock_refresh);
}

/* ---------------------------------------------------------------- counters / filter */

static lv_obj_t *s_filter_pct, *s_filter_bar, *s_filter_hint, *s_hours, *s_outages;

static void counters_refresh(const mvhr_state_t *st)
{
    if (!(st->valid & MVHR_G_COUNTERS)) {
        return;
    }
    bool due = st->filter_pct >= 90 || (st->alarms & MVHR_ALARM_CLEAN_FILTERS);
    lv_label_set_text_fmt(s_filter_pct, "%u %%", st->filter_pct);
    lv_obj_set_style_text_color(s_filter_pct, due ? COL_WARN : COL_TEXT, 0);
    lv_bar_set_value(s_filter_bar, st->filter_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_filter_bar, due ? COL_HOT : COL_ACCENT, LV_PART_INDICATOR);
    lv_label_set_text(s_filter_hint, due ? "Clean or replace the filters, then reset." : "Filters are fine.");
    lv_label_set_text_fmt(s_hours, "%lu h", (unsigned long)st->hours);
    lv_label_set_text_fmt(s_outages, "%lu", (unsigned long)st->power_outages);
}

static void do_reset_filter(void)
{
    mvhr_reset_filter();
    ui_show_counters();
}

static void reset_cb(lv_event_t *e)
{
    ui_confirm("Reset filter", "Reset the filter saturation counter to 0 %?\nDo this after cleaning the filters.",
               "Reset", do_reset_filter, ui_show_counters);
}

static void back_to_menu_cb(lv_event_t *e)
{
    ui_show_menu();
}

static lv_obj_t *kv_row(lv_obj_t *scr, int y, const char *key)
{
    lv_obj_t *k = ui_label(scr, &lv_font_montserrat_14, COL_TEXT2, key);
    lv_obj_set_pos(k, 12, y);
    lv_obj_t *v = ui_label(scr, &lv_font_montserrat_14, COL_TEXT, "-");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -12, y);
    return v;
}

void ui_show_counters(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Filter & counters", ui_show_menu);
    ui_label(scr, &lv_font_montserrat_14, COL_TEXT, "Filter saturation");
    lv_obj_set_pos(lv_obj_get_child(scr, -1), 12, TITLE_H + 12);
    s_filter_pct = ui_label(scr, &lv_font_montserrat_18, COL_TEXT, "-");
    lv_obj_align(s_filter_pct, LV_ALIGN_TOP_RIGHT, -12, TITLE_H + 9);
    s_filter_bar = lv_bar_create(scr);
    lv_obj_set_size(s_filter_bar, SCR_W - 24, 10);
    lv_obj_set_pos(s_filter_bar, 12, TITLE_H + 36);
    lv_bar_set_range(s_filter_bar, 0, 100);
    lv_obj_set_style_bg_color(s_filter_bar, COL_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_filter_bar, LV_OPA_COVER, LV_PART_MAIN);
    s_filter_hint = ui_label(scr, &lv_font_montserrat_12, COL_WARN_TEXT, "");
    lv_obj_set_pos(s_filter_hint, 12, TITLE_H + 52);
    s_hours = kv_row(scr, TITLE_H + 82, "Hours of service");
    s_outages = kv_row(scr, TITLE_H + 108, "Power outages");
    lv_obj_t *row = ui_bottom_buttons(scr, "Back", back_to_menu_cb, "Reset filter...", reset_cb, COL_WARN);
    (void)row;
    ui_load(scr, SCR_SUB, counters_refresh);
}

/* ---------------------------------------------------------------- device */

static const uint16_t s_dim_steps[] = {30, 60, 120, 300, 0};
static const uint16_t s_off_steps[] = {60, 120, 300, 600, 1800, 0};

static void fmt_duration(char *out, size_t size, uint16_t s)
{
    if (!s) {
        snprintf(out, size, "Never");
    } else if (s < 60) {
        snprintf(out, size, "%u s", s);
    } else {
        snprintf(out, size, "%u min", s / 60);
    }
}

static row_t s_row_ha, s_row_fw;
static bool s_show_password;

static void device_refresh(const mvhr_state_t *st)
{
    net_status_t n;
    net_get_status(&n);
    switch (n.state) {
    case NET_CONNECTED:
        lv_label_set_text(s_row_a.title, "Wi-Fi connected");
        lv_label_set_text_fmt(s_row_a.sub, "%s  %s.local", n.ip, settings_device_id());
        break;
    case NET_OFF:
        lv_label_set_text(s_row_a.title, "Display only");
        lv_label_set_text(s_row_a.sub, "Tap to set up Wi-Fi");
        break;
    default:
        lv_label_set_text(s_row_a.title, "Wi-Fi not connected");
        lv_label_set_text_fmt(s_row_a.sub, "%s", n.last_error[0] ? n.last_error : n.sta_ssid);
        break;
    }
    switch (ha_state()) {
    case HA_CONNECTED:
        lv_label_set_text(s_row_ha.sub, "Connected");
        break;
    case HA_CONNECTING:
        lv_label_set_text_fmt(s_row_ha.sub, "Connecting to %s", settings_get()->mqtt_host);
        break;
    default:
        lv_label_set_text_fmt(s_row_ha.sub, n.state == NET_OFF ? "Needs Wi-Fi" : "Set up at %s.local/ha",
                              settings_device_id());
        break;
    }
    char buf[16];
    fmt_duration(buf, sizeof(buf), settings_get()->dim_timeout_s);
    lv_label_set_text(s_row_b.value, buf);
    fmt_duration(buf, sizeof(buf), settings_get()->screen_timeout_s);
    lv_label_set_text(s_row_c.value, buf);
    if (s_show_password) {
        lv_label_set_text_fmt(s_row_fw.sub, "Device password: %s", settings_get()->ota_password);
    } else {
        lv_label_set_text_fmt(s_row_fw.sub, "Unit %u.%u%s. Tap for the password", st->fw_major, st->fw_minor,
                              mvhr_is_simulated() ? " (simulated)" : "");
    }
}

static void theme_cb(lv_event_t *e)
{
    settings_t s = *settings_get();
    s.dark_theme = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_save(&s);
    ui_theme_apply(s.dark_theme);
    ui_show_device();   /* rebuild in the new colours */
}

static void password_cb(lv_event_t *e)
{
    s_show_password = !s_show_password;
    mvhr_state_t st;
    mvhr_get_state(&st);
    device_refresh(&st);
}



static void cycle(uint16_t *val, const uint16_t *steps, size_t n)
{
    size_t i = 0;
    while (i < n && steps[i] != *val) {
        i++;
    }
    *val = steps[(i + 1) % n];
}

static void dim_cb(lv_event_t *e)
{
    settings_t s = *settings_get();
    cycle(&s.dim_timeout_s, s_dim_steps, sizeof(s_dim_steps) / sizeof(s_dim_steps[0]));
    settings_save(&s);
    mvhr_state_t st;
    mvhr_get_state(&st);
    device_refresh(&st);
}

static void off_cb(lv_event_t *e)
{
    settings_t s = *settings_get();
    cycle(&s.screen_timeout_s, s_off_steps, sizeof(s_off_steps) / sizeof(s_off_steps[0]));
    settings_save(&s);
    mvhr_state_t st;
    mvhr_get_state(&st);
    device_refresh(&st);
}

static void wifi_reset(void)
{
    settings_reset_network();
    esp_restart();
}

static void wifi_cb(lv_event_t *e)
{
    ui_confirm("Wi-Fi setup", "Forget the Wi-Fi settings and start\nthe setup hotspot? The device restarts.",
               "Set up", wifi_reset, ui_show_device);
}

void ui_show_device(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Device", ui_show_menu);
    lv_obj_t *list = ui_box(scr);
    lv_obj_set_size(list, SCR_W, SCR_H - TITLE_H);
    lv_obj_set_pos(list, 0, TITLE_H);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    row_create(&s_row_a, list, 0, 48, &ic_wifi_20, "Wi-Fi", "", NULL, NULL, NULL);
    lv_obj_add_flag(s_row_a.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_row_a.row, wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(ui_label(s_row_a.row, &lv_font_montserrat_14, COL_MUTED, LV_SYMBOL_RIGHT), LV_ALIGN_RIGHT_MID, -4, 0);
    row_create(&s_row_ha, list, 48, 48, &ic_home_20, "Home Assistant", "", NULL, NULL, NULL);
    row_t theme;
    row_create(&theme, list, 96, 46, &ic_moon_20, "Dark theme", NULL, NULL, NULL, NULL);
    lv_obj_set_state(themed_switch(theme.row, theme_cb), LV_STATE_CHECKED, ui_theme_dark());
    row_create(&s_row_b, list, 142, 46, &ic_sun_20, "Dim after", NULL, "-", dim_cb, NULL);
    row_create(&s_row_c, list, 188, 46, &ic_screen_off_20, "Screen off after", NULL, "-", off_cb, NULL);

    char title[48];
    snprintf(title, sizeof(title), "Firmware %s", esp_app_get_description()->version);
    row_create(&s_row_fw, list, 234, 52, &ic_info_20, title, "", NULL, NULL, NULL);
    lv_obj_set_style_border_width(s_row_fw.row, 0, 0);
    lv_obj_add_flag(s_row_fw.row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_row_fw.row, password_cb, LV_EVENT_CLICKED, NULL);
    s_show_password = false;
    ui_load(scr, SCR_SUB, device_refresh);
}
