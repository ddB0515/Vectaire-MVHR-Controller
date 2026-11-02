/* Home: clock, inside/outside climate in a house drawing, fan speed and bypass. */
#include <stdio.h>

#include "ha.h"
#include "net.h"
#include "settings.h"
#include "ui_priv.h"

#define BODY_H          118
#define FAN_PENDING_MS  4000    /* show the requested speed until the unit reports it */

static lv_obj_t *s_time, *s_date, *s_wifi, *s_ha, *s_banner, *s_banner_icon, *s_banner_text, *s_body;
static lv_obj_t *s_rh_in, *s_chip, *s_speed, *s_pips[3], *s_minus, *s_plus, *s_bypass, *s_bypass_text;
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
    if (n.state == NET_OFF) {
        lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
        bool up = n.state == NET_CONNECTED;
        lv_image_set_src(s_wifi, up ? &ic_wifi_18 : &ic_wifi_off_18);
        lv_obj_set_style_image_recolor(s_wifi, up ? COL_ACCENT : COL_MUTED, 0);
    }

    lv_obj_set_flag(s_ha, LV_OBJ_FLAG_HIDDEN, ha_state() != HA_CONNECTED);

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
    const char *fn = valid ? mvhr_function_text(st) : NULL;
    if (fn) {
        lv_label_set_text(s_chip, fn);
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
    s_ha = ui_icon(scr, &ic_home_18, COL_ACCENT);
    lv_obj_set_pos(s_ha, SCR_W - 44 - 8 - 18 - 8 - 18, 9);
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
    lv_obj_t *in = ui_box(s_body);
    lv_obj_set_width(in, 140);
    lv_obj_set_pos(in, 32, 36);
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(in, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(in, 3, 0);
    ui_temp_create(&s_t_in, in, &lv_font_montserrat_28, 7);
    lv_obj_t *rh = ui_box(in);
    lv_obj_set_flex_flow(rh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rh, 4, 0);
    ui_icon(rh, &ic_droplet_14, COL_ACCENT);
    s_rh_in = ui_label(rh, &lv_font_montserrat_14, COL_TEXT, "-- %");
    s_chip = ui_label(in, &lv_font_montserrat_12, COL_WHITE, "");
    lv_obj_set_style_bg_color(s_chip, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chip, 9, 0);
    lv_obj_set_style_pad_hor(s_chip, 8, 0);
    lv_obj_set_style_pad_ver(s_chip, 2, 0);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *out = ui_box(s_body);
    lv_obj_set_size(out, 108, 90);
    lv_obj_set_pos(out, 206, 22);
    lv_obj_set_style_border_side(out, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(out, 1, 0);
    lv_obj_set_style_border_color(out, COL_LINE, 0);
    lv_obj_set_style_pad_left(out, 8, 0);
    lv_obj_set_flex_flow(out, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(out, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(out, 3, 0);
    ui_label(out, &lv_font_montserrat_10, COL_MUTED, "OUTSIDE");
    ui_temp_create(&s_t_out, out, &lv_font_montserrat_22, 6);   /* the unit has no outside humidity sensor */

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
