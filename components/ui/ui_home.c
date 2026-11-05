/* Home: clock, inside/outside climate in a house drawing, fan speed and bypass. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ha.h"
#include "net.h"
#include "settings.h"
#include "ui_priv.h"

#define BODY_H          118
#define FAN_PENDING_MS  4000    /* show the requested speed until the unit reports it */

static lv_obj_t *s_time, *s_date, *s_wifi, *s_ha, *s_ha_glyph, *s_banner, *s_banner_icon, *s_banner_text, *s_body;
static lv_obj_t *s_rh_in, *s_airflow, *s_airflow_icon, *s_fan_in, *s_fan_out, *s_fan_in_box, *s_fan_out_box, *s_chip, *s_chip_text, *s_speed, *s_pips[3], *s_minus, *s_plus, *s_bypass, *s_bypass_text;
static ui_temp_t s_t_in, s_t_out;
static uint16_t s_banner_bit;
static int s_fan_pending;
static uint32_t s_fan_pending_ms;

static const lv_point_precise_t s_roof[] = {{12, 50}, {102, 8}, {192, 50}};
static const lv_point_precise_t s_walls[] = {{32, 41}, {32, 114}, {172, 114}, {172, 41}};

static uint8_t weekday(int y, int m, int d)     /* 0 = Sunday (Sakamoto) */
{
    static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y--;
    }
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7);
}

static void banner_cb(lv_event_t *e)
{
    if (s_banner_bit) {
        ui_show_alarm(s_banner_bit);
    }
}

static void fan_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    mvhr_state_t st;
    mvhr_get_state(&st);
    int cur = lv_tick_elaps(s_fan_pending_ms) < FAN_PENDING_MS && s_fan_pending ? s_fan_pending : st.fan_speed;
    int want = cur + delta;
    if (want < 1 || want > 3) {
        return;
    }
    s_fan_pending = want;
    s_fan_pending_ms = lv_tick_get();
    mvhr_set_fan_speed((uint8_t)want);
    mvhr_refresh(MVHR_G_STATUS);
    lv_label_set_text_fmt(s_speed, "Speed %d", want);
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_pips[i], i < want ? COL_ACCENT : COL_BORDER, 0);
    }
}

static void home_refresh(const mvhr_state_t *st)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (st->valid & MVHR_G_CLOCK) {
        const mvhr_clock_t *c = &st->clock;
        if (c->format_24h) {
            lv_label_set_text_fmt(s_time, "%02u:%02u", c->hour, c->minute);
        } else {
            lv_label_set_text_fmt(s_time, "%u:%02u %s", c->hour % 12 ? c->hour % 12 : 12, c->minute,
                                  c->hour < 12 ? "am" : "pm");
        }
        if (c->month >= 1 && c->month <= 12) {
            lv_label_set_text_fmt(s_date, "%s %u %s", days[weekday(2000 + c->year, c->month, c->day)], c->day,
                                  months[c->month - 1]);
        }
    }

    net_status_t n;
    net_get_status(&n);
    /* shown only while connected */
    lv_obj_set_flag(s_wifi, LV_OBJ_FLAG_HIDDEN, n.state != NET_CONNECTED);

    bool ha_up = ha_state() == HA_CONNECTED;
    lv_obj_set_flag(s_ha, LV_OBJ_FLAG_HIDDEN, !ha_up);
    lv_obj_set_flag(s_ha_glyph, LV_OBJ_FLAG_HIDDEN, !ha_up);

    /* banner: unit offline, else the most important alarm */
    const ui_alarm_info_t *a = st->online ? ui_alarm_top(st->alarms) : NULL;
    bool offline = !st->online;
    s_banner_bit = a ? a->bit : 0;
    if (offline || a) {
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_color_t bg = offline ? COL_PANEL : a->kind == ALARM_FAIL ? COL_FAIL_BG : COL_WARN_BG;
        lv_color_t fg = offline ? COL_TEXT2 : a->kind == ALARM_FAIL ? COL_FAIL_TEXT : COL_WARN_TEXT;
        lv_obj_set_style_bg_color(s_banner, bg, 0);
        lv_obj_set_style_text_color(s_banner, fg, 0);
        lv_obj_set_style_image_recolor(s_banner_icon, offline ? COL_MUTED : a->kind == ALARM_FAIL ? COL_FAIL : COL_WARN, 0);
        lv_label_set_text(s_banner_text, offline ? "Unit not responding" : a->headline);
        lv_obj_set_y(s_body, 62);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_body, 36 + (144 - BODY_H) / 2);
    }
    /* the fan readings above the roof share the banner's strip */
    lv_obj_set_flag(s_fan_in_box, LV_OBJ_FLAG_HIDDEN, offline || a);
    lv_obj_set_flag(s_fan_out_box, LV_OBJ_FLAG_HIDDEN, offline || a);

    bool valid = st->online && (st->valid & MVHR_G_STATUS);
    ui_temp_set(&s_t_in, valid, st->temp_extract_x10);
    ui_temp_set(&s_t_out, valid, st->temp_supply_x10);
    /* like the original panel: a number only within 35..99 %, otherwise "Low" or "High" */
    if (!valid || st->humidity_x10 > MVHR_RH_VALID_MAX_X10) {
        lv_label_set_text(s_rh_in, "-- %");
    } else if (st->humidity_x10 < MVHR_RH_SHOWN_MIN_X10) {
        lv_label_set_text(s_rh_in, "Low");
    } else if (st->humidity_x10 > MVHR_RH_SHOWN_MAX_X10) {
        lv_label_set_text(s_rh_in, "High");
    } else {
        lv_label_set_text_fmt(s_rh_in, "%u %%", (st->humidity_x10 + 5) / 10);
    }
    const char *flow = valid ? mvhr_airflow_text(st) : "--";
    lv_label_set_text(s_airflow, flow);
    /* heat recovery orange, bypass blue; intake motor off keeps grey text with a green icon */
    int flow_mode = mvhr_airflow_mode(st);
    lv_color_t flow_col = !valid ? COL_MUTED : flow_mode == 0 ? COL_HOT : flow_mode == 1 ? COL_ACCENT : COL_OK;
    lv_obj_set_style_text_color(s_airflow, valid && flow_mode == 2 ? COL_MUTED : flow_col, 0);
    static const lv_image_dsc_t *const flow_icons[] = {&ic_flow_hr_28, &ic_flow_bypass_28, &ic_flow_off_28};
    lv_image_set_src(s_airflow_icon, flow_icons[flow_mode]);
    lv_obj_set_style_image_recolor(s_airflow_icon, flow_col, 0);
    lv_obj_set_flag(s_airflow_icon, LV_OBJ_FLAG_HIDDEN, !valid);
    if (valid) {
        lv_label_set_text_fmt(s_fan_in, "%u%%", st->fan_a_pct);
        lv_label_set_text_fmt(s_fan_out, "%u%%", st->fan_b_pct);
    } else {
        lv_label_set_text(s_fan_in, "--");
        lv_label_set_text(s_fan_out, "--");
    }
    const char *fn = valid ? mvhr_function_text(st) : NULL;
    if (fn) {
        char tag[24];
        snprintf(tag, sizeof(tag), "%s", !strcmp(fn, "Boost") ? "Boost active" : fn);   /* HA and the web keep "Boost" */
        for (char *c = tag; *c; c++) {
            *c = (char)toupper((unsigned char)*c);
        }
        lv_label_set_text(s_chip_text, tag);
        lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    }

    int speed = st->fan_speed;
    if (s_fan_pending && lv_tick_elaps(s_fan_pending_ms) < FAN_PENDING_MS && speed != s_fan_pending) {
        speed = s_fan_pending;
    } else {
        s_fan_pending = 0;
    }
    if (valid) {
        lv_label_set_text_fmt(s_speed, "Speed %d", speed);
    } else {
        lv_label_set_text(s_speed, "Speed -");
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_pips[i], valid && i < speed ? COL_ACCENT : COL_BORDER, 0);
    }
    lv_obj_set_state(s_minus, LV_STATE_DISABLED, !valid || speed <= 1);
    lv_obj_set_state(s_plus, LV_STATE_DISABLED, !valid || speed >= 3);
    lv_obj_set_state(s_bypass, LV_STATE_DISABLED, !valid);

    static const char *bypass[] = {"Heat rec.", "Bypass", "Intake off"};
    lv_label_set_text(s_bypass_text, st->bypass_override == MVHR_BYPASS_AUTO ? "Auto"
                                     : st->bypass_override <= 2 ? bypass[st->bypass_override] : "?");
}

static lv_obj_t *small_button(lv_obj_t *parent, int x, int w, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(parent);
    ui_style_button(b, false);
    lv_obj_set_pos(b, x, 7);
    lv_obj_set_size(b, w, 46);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

void ui_show_home(void)
{
    lv_obj_t *scr = ui_screen();

    /* header */
    /* a flex row, so the date moves along when the time gets wider */
    lv_obj_t *clock = ui_box(scr);
    lv_obj_set_pos(clock, 10, 9);
    lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(clock, 10, 0);
    s_time = ui_label(clock, &lv_font_montserrat_18, COL_TEXT, "--:--");
    s_date = ui_label(clock, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_style_pad_bottom(s_date, 2, 0);
    s_wifi = ui_icon(scr, &ic_wifi_18, COL_ACCENT);
    lv_obj_set_pos(s_wifi, SCR_W - 44 - 8 - 18, 9);
    /* Home Assistant logo: blue house with the light glyph on top */
    s_ha = ui_icon(scr, &ic_ha_house_18, lv_color_hex(0x18bcf2));
    lv_obj_set_pos(s_ha, SCR_W - 44 - 8 - 18 - 8 - 18, 9);
    s_ha_glyph = ui_icon(scr, &ic_ha_glyph_18, lv_color_hex(0xf2f4f9));
    lv_obj_set_pos(s_ha_glyph, SCR_W - 44 - 8 - 18 - 8 - 18, 9);
    lv_obj_t *gear = lv_button_create(scr);
    ui_style_button(gear, true);
    lv_obj_set_style_radius(gear, 0, 0);
    lv_obj_set_size(gear, 44, 36);
    lv_obj_set_pos(gear, SCR_W - 44, 0);
    lv_obj_add_event_cb(gear, ui_go_cb, LV_EVENT_CLICKED, (void *)ui_show_menu);
    lv_obj_center(ui_icon(gear, &ic_settings_22, COL_WHITE));
    lv_obj_t *line = ui_box(scr);
    lv_obj_set_size(line, SCR_W, 1);
    lv_obj_set_pos(line, 0, 35);
    lv_obj_set_style_bg_color(line, COL_LINE, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    /* alarm / offline banner */
    s_banner = lv_button_create(scr);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_size(s_banner, SCR_W, 26);
    lv_obj_set_pos(s_banner, 0, 36);
    lv_obj_add_event_cb(s_banner, banner_cb, LV_EVENT_CLICKED, NULL);
    s_banner_icon = ui_icon(s_banner, &ic_alert_16, COL_WARN);
    lv_obj_align(s_banner_icon, LV_ALIGN_LEFT_MID, 10, 0);
    s_banner_text = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_text, &lv_font_montserrat_12, 0);
    lv_obj_align(s_banner_text, LV_ALIGN_LEFT_MID, 32, 0);
    lv_obj_t *chev = lv_label_create(s_banner);
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_12, 0);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    /* body: house with the inside values, outside values on the right */
    s_body = ui_box(scr);
    lv_obj_set_size(s_body, SCR_W, BODY_H);
    lv_obj_set_pos(s_body, 0, 49);
    const lv_point_precise_t *shapes[] = {s_roof, s_walls};
    const uint32_t counts[] = {3, 4};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_line_create(s_body);
        lv_line_set_points(l, shapes[i], counts[i]);
        lv_obj_set_style_line_width(l, 5, 0);
        lv_obj_set_style_line_color(l, COL_HOUSE, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
    }
    /* inside the house: mode icon under the apex with its name, temperature and humidity along the bottom */
    lv_obj_t *in = ui_box(s_body);
    lv_obj_set_size(in, 140, 92);
    lv_obj_set_pos(in, 32, 19);
    lv_obj_set_style_pad_top(in, 9, 0);     /* nudges the mode group down, nearer the middle */
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(in, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *mode = ui_box(in);
    lv_obj_set_flex_flow(mode, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mode, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mode, 1, 0);
    s_airflow_icon = ui_icon(mode, &ic_flow_hr_28, COL_MUTED);
    s_airflow = ui_label(mode, &lv_font_montserrat_14, COL_MUTED, "--");
    lv_obj_t *row = ui_box(in);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    /* thermometer + temperature, droplet + humidity, both at the same size */
    lv_obj_t *tg = ui_box(row);
    lv_obj_set_flex_flow(tg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tg, 2, 0);
    ui_icon(tg, &ic_thermo_16, COL_ACCENT);
    ui_temp_create(&s_t_in, tg, &lv_font_montserrat_18, 4);
    lv_obj_t *rh = ui_box(row);
    lv_obj_set_flex_flow(rh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rh, 2, 0);
    ui_icon(rh, &ic_droplet_16, COL_ACCENT);
    s_rh_in = ui_label(rh, &lv_font_montserrat_18, COL_TEXT, "-- %");

    lv_obj_t *out = ui_box(s_body);
    lv_obj_set_size(out, 118, 90);
    lv_obj_set_pos(out, 200, 10);     /* 59 px from the top of the screen (body starts at 49) */
    lv_obj_set_style_border_side(out, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(out, 1, 0);
    lv_obj_set_style_border_color(out, COL_LINE, 0);
    lv_obj_set_style_pad_left(out, 8, 0);
    lv_obj_set_flex_flow(out, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(out, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);   /* label, temperature and tag centred in the column, as in the preview */
    lv_obj_set_style_pad_row(out, 3, 0);
    ui_label(out, &lv_font_montserrat_10, COL_MUTED, "OUTDOOR TEMP");
    ui_temp_create(&s_t_out, out, &lv_font_montserrat_22, 6);   /* the unit has no outside humidity sensor */

    /* active function (boost, night mode...) as a tag as wide as the column, in capitals */
    s_chip = ui_box(out);
    lv_obj_set_size(s_chip, LV_PCT(100), 46);
    lv_obj_set_style_bg_color(s_chip, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chip, 8, 0);
    lv_obj_set_style_pad_hor(s_chip, 5, 0);     /* padding 0 5 px around the text */
    lv_obj_set_style_pad_ver(s_chip, 0, 0);
    lv_obj_set_flex_flow(s_chip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_chip_text = ui_label(s_chip, &lv_font_montserrat_14, COL_WHITE, "");
    lv_obj_set_width(s_chip_text, LV_PCT(100));
    lv_obj_set_style_text_align(s_chip_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);

    /* intake fan above the left half of the roof, exhaust fan above the right half */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *box = ui_box(scr);
        lv_obj_set_size(box, 90, 32);
        lv_obj_set_pos(box, i ? 102 : 8, 42);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        /* the track must be pushed right too, otherwise the lines only line up with each other, not with the box edge */
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, i ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              i ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
        ui_label(box, &lv_font_montserrat_12, COL_MUTED, i ? "Exhaust fan:" : "Intake fan:");
        lv_obj_t *val = ui_label(box, &lv_font_montserrat_14, COL_TEXT, "--");
        if (i) {
            s_fan_out_box = box;
            s_fan_out = val;
        } else {
            s_fan_in_box = box;
            s_fan_in = val;
        }
    }

    /* footer: fan speed and bypass */
    lv_obj_t *foot = ui_box(scr);
    lv_obj_set_size(foot, SCR_W, 60);
    lv_obj_set_pos(foot, 0, 180);
    lv_obj_set_style_bg_color(foot, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, COL_LINE, 0);

    s_minus = small_button(foot, 8, 48, fan_cb, (void *)(intptr_t)-1);
    lv_obj_center(ui_label(s_minus, &lv_font_montserrat_18, COL_TEXT, LV_SYMBOL_MINUS));
    lv_obj_t *mid = ui_box(foot);
    lv_obj_set_size(mid, 118, 46);
    lv_obj_set_pos(mid, 62, 7);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 8, 0);
    ui_icon(mid, &ic_fan_26, COL_ACCENT);
    lv_obj_t *col = ui_box(mid);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    s_speed = ui_label(col, &lv_font_montserrat_14, COL_TEXT, "Speed -");
    lv_obj_t *pips = ui_box(col);
    lv_obj_set_flex_flow(pips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(pips, 3, 0);
    for (int i = 0; i < 3; i++) {
        s_pips[i] = ui_box(pips);
        lv_obj_set_size(s_pips[i], 10, 6 + 3 * i);
        lv_obj_set_style_radius(s_pips[i], 1, 0);
        lv_obj_set_style_bg_opa(s_pips[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_pips[i], COL_BORDER, 0);
    }
    s_plus = small_button(foot, 186, 48, fan_cb, (void *)(intptr_t)1);
    lv_obj_center(ui_label(s_plus, &lv_font_montserrat_18, COL_TEXT, LV_SYMBOL_PLUS));
    s_bypass = small_button(foot, 240, 72, ui_go_cb, (void *)ui_show_bypass);
    lv_obj_set_flex_flow(s_bypass, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_bypass, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_bypass, 1, 0);
    ui_icon(s_bypass, &ic_exchange_24, COL_ACCENT);
    s_bypass_text = ui_label(s_bypass, &lv_font_montserrat_10, COL_TEXT, "Auto");

    s_fan_pending = 0;
    ui_load(scr, SCR_HOME, home_refresh);
}
