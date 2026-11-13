/* Alarm screens: one per active alarm (texts follow the original panel), and the fire alarm. */
#include "ui_priv.h"

#define SUPPORT "Please contact technical support."

/* most important first */
static const ui_alarm_info_t s_alarms[] = {
    {MVHR_ALARM_DRAIN_PAN_FULL, ALARM_WARN, "Warning", "Condensate drain pan full",
     "The system is out of service. Switch off the power supply and empty the drain pan.", NULL, NULL},
    {MVHR_ALARM_INTAKE_FAN_1, ALARM_FAIL, "System failure", "Intake fan fault (MTR-1)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_EXHAUST_FAN_1, ALARM_FAIL, "System failure", "Exhaust fan fault (MTR-1)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_INTAKE_FAN_2, ALARM_FAIL, "System failure", "Intake fan fault (MTR-2)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_EXHAUST_FAN_2, ALARM_FAIL, "System failure", "Exhaust fan fault (MTR-2)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_NTC1, ALARM_FAIL, "System failure", "Temperature probe fault (NTC1)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_NTC2, ALARM_FAIL, "System failure", "Temperature probe fault (NTC2)", SUPPORT, NULL, NULL},
    {MVHR_ALARM_RTC, ALARM_FAIL, "System failure", "Real-time clock defective", SUPPORT, NULL, NULL},
    {MVHR_ALARM_CLOCK_BATTERY, ALARM_MAINT, "Maintenance notice", "Clock battery is low",
     "The clock battery in the unit has to be replaced.", NULL, NULL},
    {MVHR_ALARM_CLOCK_INACCURATE, ALARM_MAINT, "Maintenance notice", "Clock needs setting",
     "The unit's date and time are inaccurate. Please set the clock.", "Set clock", ui_show_clock},
    {MVHR_ALARM_CLEAN_FILTERS, ALARM_MAINT, "Maintenance notice", "Clean the filters",
     "Filter saturation has reached its limit. After cleaning, reset the counter in Menu > Filter.", "Go to Filter",
     ui_show_counters},
};

const ui_alarm_info_t *ui_alarm_top(uint16_t alarms)
{
    for (size_t i = 0; i < sizeof(s_alarms) / sizeof(s_alarms[0]); i++) {
        if (alarms & s_alarms[i].bit) {
            return &s_alarms[i];
        }
    }
    return NULL;
}

static const ui_alarm_info_t *find(uint16_t bit)
{
    for (size_t i = 0; i < sizeof(s_alarms) / sizeof(s_alarms[0]); i++) {
        if (s_alarms[i].bit == bit) {
            return &s_alarms[i];
        }
    }
    return NULL;
}

static const ui_alarm_info_t *s_shown;

static void hide_cb(lv_event_t *e)
{
    ui_show_home();
}

static void action_cb(lv_event_t *e)
{
    if (s_shown && s_shown->go) {
        s_shown->go();
    }
}

static void alarm_refresh(const mvhr_state_t *st)
{
    if (s_shown && st->online && !(st->alarms & s_shown->bit)) {
        ui_show_home();     /* cleared */
    }
}

void ui_show_alarm(uint16_t bit)
{
    const ui_alarm_info_t *a = find(bit);
    if (!a) {
        return;
    }
    s_shown = a;
    bool fail = a->kind == ALARM_FAIL;
    lv_obj_t *scr = ui_screen();

    lv_obj_t *head = ui_box(scr);
    lv_obj_set_size(head, SCR_W, 52);
    lv_obj_set_style_bg_color(head, fail ? COL_FAIL_BG : COL_WARN_BG, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 2, 0);
    lv_obj_set_style_border_color(head, fail ? COL_FAIL : COL_WARN_LINE, 0);
    const lv_image_dsc_t *icon = fail ? &ic_failure_32 : a->kind == ALARM_MAINT ? &ic_tool_32 : &ic_alert_32;
    lv_obj_align(ui_icon(head, icon, fail ? COL_FAIL : COL_WARN), LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_align(ui_label(head, &lv_font_montserrat_18, fail ? COL_FAIL_TEXT : COL_WARN_TEXT, a->title),
                 LV_ALIGN_LEFT_MID, 54, 0);

    lv_obj_t *col = ui_box(scr);
    lv_obj_set_size(col, SCR_W - 24, 124);
    lv_obj_set_pos(col, 12, 56);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_t *h = ui_label(col, &lv_font_montserrat_16, COL_TEXT, a->headline);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *t = ui_label(col, &lv_font_montserrat_12, COL_TEXT2, a->text);
    lv_obj_set_width(t, SCR_W - 24);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    ui_bottom_buttons(scr, "Hide", hide_cb, a->action, a->action ? action_cb : NULL, COL_ACCENT);
    ui_load(scr, SCR_ALARM, alarm_refresh);
}

/* ---------------------------------------------------------------- fire */

static lv_obj_t *s_resume, *s_wait;

static void resume_cb(lv_event_t *e)
{
    mvhr_fire_alarm_resume();
}

static void fire_refresh(const mvhr_state_t *st)
{
    if (st->online && !(st->alarms & MVHR_ALARM_FIRE)) {
        ui_show_home();
        return;
    }
    /* RESUME only once the fire input itself has cleared (reply bit 1 = still active) */
    bool active = (st->valid & MVHR_G_FIRE) && (st->fire_reply & 0x02);
    lv_obj_set_flag(s_resume, LV_OBJ_FLAG_HIDDEN, active);
    lv_obj_set_flag(s_wait, LV_OBJ_FLAG_HIDDEN, !active);
}

void ui_show_fire(void)
{
    lv_obj_t *scr = ui_screen();
    lv_obj_set_style_bg_color(scr, COL_FIRE, 0);
    lv_obj_t *icon = ui_icon(scr, &ic_bell_64, COL_WHITE);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_t *t = ui_label(scr, &lv_font_montserrat_22, COL_WHITE, "FIRE ALARM");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *s = ui_label(scr, &lv_font_montserrat_14, COL_WHITE, "The ventilation has stopped.");
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 132);
    s_resume = lv_button_create(scr);
    ui_style_button(s_resume, false);
    lv_obj_set_style_border_width(s_resume, 0, 0);
    lv_obj_set_size(s_resume, 240, 50);
    lv_obj_align(s_resume, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(s_resume, resume_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ui_label(s_resume, &lv_font_montserrat_18, COL_FIRE, "RESUME"));
    s_wait = ui_label(scr, &lv_font_montserrat_12, COL_WHITE, "Waiting for the fire alarm input to clear.");
    lv_obj_align(s_wait, LV_ALIGN_BOTTOM_MID, 0, -28);
    ui_load(scr, SCR_FIRE, fire_refresh);
}
