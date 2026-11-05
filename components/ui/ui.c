/* UI start-up, first-start screens, and the tick that refreshes screens, raises alarms and sleeps the display. */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "net.h"
#include "settings.h"
#include "ui_priv.h"

static const char *TAG = "ui";

#define TICK_MS             250
#define BACK_HOME_MS        60000   /* other screens fall back to home without a touch */
#define CONNECTED_SHOW_MS   3000

void ui_refresh_current(const mvhr_state_t *st);   /* ui_common.c */

/* ---------------------------------------------------------------- sleep / wake */

typedef enum { LIGHT_ON, LIGHT_DIM, LIGHT_OFF } light_t;

static light_t s_light = LIGHT_ON;
static lv_obj_t *s_shield;      /* swallows the waking tap so it can't press anything */

void ui_wake(void)
{
    lv_display_trigger_activity(NULL);
    if (s_light != LIGHT_ON) {
        s_light = LIGHT_ON;
        board_backlight_set(settings_get()->backlight_pct);
    }
    if (s_shield) {
        lv_obj_delete_async(s_shield);
        s_shield = NULL;
    }
}

static void shield_cb(lv_event_t *e)
{
    ui_wake();
}

static void go_dark(light_t level)
{
    if (!s_shield) {
        s_shield = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, SCR_W, SCR_H);
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESSED, NULL);
    }
    s_light = level;
    board_backlight_set(level == LIGHT_DIM ? settings_get()->dim_pct : 0);
}

static void sleep_tick(void)
{
    ui_screen_t cur = ui_current();
    if (cur == SCR_CHOICE || cur == SCR_PROVISION || cur == SCR_FIRE) {
        return;
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    const settings_t *s = settings_get();
    if (s->screen_timeout_s && idle >= s->screen_timeout_s * 1000U) {
        if (s_light != LIGHT_OFF) {
            go_dark(LIGHT_OFF);
        }
    } else if (s->dim_timeout_s && idle >= s->dim_timeout_s * 1000U) {
        if (s_light == LIGHT_ON) {
            go_dark(LIGHT_DIM);
        }
    }
    if ((cur == SCR_MENU || cur == SCR_SUB || cur == SCR_EDIT || cur == SCR_ALARM) && idle >= BACK_HOME_MS) {
        ui_show_home();
    }
}

/* ---------------------------------------------------------------- alarms */

static bool s_alarms_known;
static uint16_t s_alarms_seen;

/* The fire alarm always takes over. Other alarms pop up when they appear (not those already active at start-up;
   the home banner shows those). */
static void alarm_tick(const mvhr_state_t *st)
{
    ui_screen_t cur = ui_current();
    if (!st->online || !(st->valid & MVHR_G_STATUS) || cur == SCR_CHOICE || cur == SCR_PROVISION) {
        return;
    }
    uint16_t al = st->alarms;
    if ((al & MVHR_ALARM_FIRE) && cur != SCR_FIRE) {
        ui_wake();
        ui_show_fire();
    } else if (s_alarms_known) {
        uint16_t fresh = al & ~s_alarms_seen & ~MVHR_ALARM_FIRE;
        const ui_alarm_info_t *a = ui_alarm_top(fresh);
        if (a && cur != SCR_FIRE && cur != SCR_EDIT) {
            ui_wake();
            ui_show_alarm(a->bit);
        }
    }
    s_alarms_known = true;
    s_alarms_seen = al;
}

static void tick_cb(lv_timer_t *t)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    alarm_tick(&st);
    sleep_tick();
    mvhr_get_state(&st);
    ui_refresh_current(&st);
}

/* ---------------------------------------------------------------- first start choice */

static void choose_wifi(lv_event_t *e)
{
    ui_show_provisioning();
    net_start_provisioning();
}

static void choose_display_only(lv_event_t *e)
{
    settings_t s = *settings_get();
    s.mode = SETTINGS_MODE_DISPLAY_ONLY;
    settings_save(&s);
    ui_show_home();
}

void ui_show_setup_choice(void)
{
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, "Welcome", NULL);
    lv_obj_t *sub = ui_label(scr, &lv_font_montserrat_14, COL_TEXT2,
                             "Connect to Wi-Fi to use Home Assistant,\nor use this controller on its own.");
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, TITLE_H + 16);

    lv_obj_t *b1 = ui_button(scr, LV_SYMBOL_WIFI "  Wi-Fi & Home Assistant", true, choose_wifi, NULL);
    lv_obj_set_size(b1, 290, 56);
    lv_obj_align(b1, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_t *b2 = ui_button(scr, "Display only", false, choose_display_only, NULL);
    lv_obj_set_size(b2, 290, 48);
    lv_obj_align(b2, LV_ALIGN_TOP_MID, 0, 176);
    ui_load(scr, SCR_CHOICE, NULL);
}

/* ---------------------------------------------------------------- provisioning (QR) */

static lv_obj_t *s_qr, *s_step, *s_detail, *s_status;
static bool s_qr_shows_url;
static lv_timer_t *s_continue_timer;

static void cancel_provisioning(lv_event_t *e)
{
    /* keep it simple and robust: restart into the saved mode */
    esp_restart();
}

static void continue_to_home(lv_timer_t *t)
{
    s_continue_timer = NULL;    /* one-shot timer deletes itself */
    if (ui_current() == SCR_PROVISION) {
        ui_show_home();
    }
}

static void show_qr(const char *data)
{
    lv_qrcode_update(s_qr, data, strlen(data));
}

static void provisioning_refresh(const net_status_t *st)
{
    if (ui_current() != SCR_PROVISION) {
        return;
    }
    if (st->state == NET_CONNECTED) {
        lv_label_set_text(s_step, "Connected!");
        lv_label_set_text_fmt(s_detail, "%s\nIP %s\n%s.local", st->sta_ssid, st->ip, settings_device_id());
        lv_obj_set_style_text_color(s_status, COL_OK, 0);
        lv_label_set_text(s_status, "Setup complete");
        if (!s_continue_timer) {    /* show the result briefly, then go on by itself */
            s_continue_timer = lv_timer_create(continue_to_home, CONNECTED_SHOW_MS, NULL);
            lv_timer_set_repeat_count(s_continue_timer, 1);
        }
        return;
    }
    if (!st->phone_joined) {
        s_qr_shows_url = false;
        char wifi_qr[96];
        snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:S:%s;T:WPA;P:%s;;", st->ap_ssid, st->ap_pass);
        show_qr(wifi_qr);
        lv_label_set_text(s_step, "Scan to join");
        lv_label_set_text_fmt(s_detail, "Network\n%s\nPassword\n%s", st->ap_ssid, st->ap_pass);
    } else if (!s_qr_shows_url) {
        s_qr_shows_url = true;
        show_qr("http://192.168.4.1/");
        lv_label_set_text(s_step, "Open setup");
        lv_label_set_text(s_detail, "The setup page\nshould open by\nitself, or scan\nthis code.");
    }
    if (st->state == NET_TESTING) {
        lv_obj_set_style_text_color(s_status, COL_MUTED, 0);
        lv_label_set_text_fmt(s_status, "Connecting to %s...", st->sta_ssid);
    } else if (st->last_error[0]) {
        lv_obj_set_style_text_color(s_status, COL_FAIL, 0);
        lv_label_set_text_fmt(s_status, "%s: %s", st->sta_ssid, st->last_error);
    } else {
        lv_obj_set_style_text_color(s_status, COL_MUTED, 0);
        lv_label_set_text(s_status, st->phone_joined ? "Phone connected" : "Waiting for phone...");
    }
}

void ui_show_provisioning(void)
{
    lv_obj_t *scr = ui_screen();

    /* bordered white box gives the QR code its quiet zone */
    lv_obj_t *box = ui_box(scr);
    lv_obj_set_style_bg_color(box, COL_WHITE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, COL_LINE, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_size(box, 176, 176);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 10, 10);
    s_qr = lv_qrcode_create(box);
    lv_qrcode_set_size(s_qr, 156);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_center(s_qr);
    s_qr_shows_url = false;

    s_step = ui_label(scr, &lv_font_montserrat_20, COL_TEXT, "Starting...");
    lv_obj_align(s_step, LV_ALIGN_TOP_LEFT, 196, 14);
    s_detail = ui_label(scr, &lv_font_montserrat_14, COL_TEXT2, "");
    lv_obj_align(s_detail, LV_ALIGN_TOP_LEFT, 196, 46);
    s_status = ui_label(scr, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(s_status, 300);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, 10, -12);

    lv_obj_t *cancel = ui_button(scr, "Cancel", false, cancel_provisioning, NULL);
    lv_obj_set_size(cancel, 110, 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -8, -40);
    ui_load(scr, SCR_PROVISION, NULL);

    net_status_t st;
    net_get_status(&st);
    if (st.ap_ssid[0]) {
        provisioning_refresh(&st);
    }
}

/* ---------------------------------------------------------------- start */

static void net_changed(const net_status_t *st, void *arg)
{
    if (!lvgl_port_lock(100)) {
        return;
    }
    provisioning_refresh(st);
    lvgl_port_unlock();
}

void ui_start(void)
{
    net_set_listener(net_changed, NULL);
    lvgl_port_lock(0);
    ui_theme_apply(settings_get()->dark_theme);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    if (settings_get()->mode == SETTINGS_MODE_UNSET) {
        ui_show_setup_choice();
    } else {
        ui_show_home();
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI started (mode %d)", settings_get()->mode);
}
