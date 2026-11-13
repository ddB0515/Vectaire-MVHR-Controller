#pragma once
/* Shared by the UI files: theme, widget helpers and screen navigation. LVGL task only. */
#include <stdbool.h>
#include <stdint.h>

#include "icons.h"
#include "lvgl.h"
#include "mvhr.h"

/* Theme: light (close to the original panel) or dark, switched at runtime (ui_theme_apply) */
typedef struct {
    lv_color_t bg, panel, tile, line, border, house, text, text2, muted, accent, accent_dn, accent_bg, warn, warn_bg, warn_line, warn_text, hot, fail, fail_bg, fail_text, ok;
} ui_palette_t;

extern const ui_palette_t *g_ui_pal;

#define COL_BG          (g_ui_pal->bg)
#define COL_PANEL       (g_ui_pal->panel)
#define COL_TILE        (g_ui_pal->tile)
#define COL_LINE        (g_ui_pal->line)
#define COL_BORDER      (g_ui_pal->border)
#define COL_HOUSE       (g_ui_pal->house)
#define COL_TEXT        (g_ui_pal->text)
#define COL_TEXT2       (g_ui_pal->text2)
#define COL_MUTED       (g_ui_pal->muted)
#define COL_ACCENT      (g_ui_pal->accent)
#define COL_ACCENT_DN   (g_ui_pal->accent_dn)
#define COL_ACCENT_BG   (g_ui_pal->accent_bg)
#define COL_WARN        (g_ui_pal->warn)
#define COL_WARN_BG     (g_ui_pal->warn_bg)
#define COL_WARN_LINE   (g_ui_pal->warn_line)
#define COL_WARN_TEXT   (g_ui_pal->warn_text)
#define COL_HOT         (g_ui_pal->hot)
#define COL_FAIL        (g_ui_pal->fail)
#define COL_FAIL_BG     (g_ui_pal->fail_bg)
#define COL_FAIL_TEXT   (g_ui_pal->fail_text)
#define COL_OK          (g_ui_pal->ok)
#define COL_FIRE        lv_color_hex(0xb71c1c)  /* same in both themes */
#define COL_WHITE       lv_color_hex(0xffffff)

/* Switch palettes, restyle the shared button styles. The caller rebuilds the visible screen. */
void ui_theme_apply(bool dark);
bool ui_theme_dark(void);

#define TITLE_H         36
#define SCR_W           320
#define SCR_H           240

typedef enum {
    SCR_NONE, SCR_CHOICE, SCR_PROVISION, SCR_HOME, SCR_MENU, SCR_SUB, SCR_EDIT, SCR_ALARM, SCR_FIRE,
} ui_screen_t;

typedef void (*ui_refresh_fn)(const mvhr_state_t *st);
typedef void (*ui_back_fn)(void);

/* Load a new screen (the old one is deleted). refresh is called with fresh state every UI tick. */
void ui_load(lv_obj_t *scr, ui_screen_t id, ui_refresh_fn refresh);
ui_screen_t ui_current(void);

/* Helpers (ui_common.c) */
lv_obj_t *ui_screen(void);
lv_obj_t *ui_box(lv_obj_t *parent);                     /* plain container, no style, not clickable */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *text);
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, lv_color_t col);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *arg);
lv_obj_t *ui_icon_button(lv_obj_t *parent, const lv_image_dsc_t *img, lv_event_cb_t cb, void *arg);
lv_obj_t *ui_title_bar(lv_obj_t *scr, const char *title, ui_back_fn back);
lv_obj_t *ui_bottom_buttons(lv_obj_t *scr, const char *left, lv_event_cb_t left_cb, const char *right,
                            lv_event_cb_t right_cb, lv_color_t right_col);
void ui_style_button(lv_obj_t *b, bool primary);

/* Temperature with a drawn degree ring (the built-in fonts have no degree sign) */
typedef struct {
    lv_obj_t *box, *value, *ring;
} ui_temp_t;
void ui_temp_create(ui_temp_t *t, lv_obj_t *parent, const lv_font_t *font, int ring);
void ui_temp_set(ui_temp_t *t, bool valid, int16_t x10);

/* Clicks go to a ui_back_fn passed as user data */
void ui_go_cb(lv_event_t *e);

/* Screens */
void ui_show_home(void);
void ui_show_menu(void);
void ui_show_bypass(void);
void ui_show_holiday(void);
void ui_show_night(void);
void ui_show_humidity(void);
void ui_show_timers(void);
void ui_show_clock(void);
void ui_show_counters(void);
void ui_show_device(void);
void ui_show_fan_speeds(void);
void ui_show_alarm(uint16_t bit);
void ui_show_fire(void);

/* Generic editors (ui_editor.c). on_set gets the value(s); back is shown afterwards. */
typedef void (*ui_value_set_fn)(int value);
void ui_edit_value(const char *title, const char *prompt, int value, int min, int max, int step, const char *fmt,
                   ui_value_set_fn on_set, ui_back_fn back);

void ui_edit_tenths(const char *title, const char *prompt, int value_x10, int min_x10, int max_x10, int step_x10,
                    ui_value_set_fn on_set, ui_back_fn back);

#define UI_MAX_FIELDS 3
typedef void (*ui_fields_set_fn)(const int *values);
typedef struct {
    int value, min, max, step;
    const char *fmt;            /* "%02d" */
} ui_field_t;
void ui_edit_fields(const char *title, const ui_field_t *fields, int n, const char *sep, ui_fields_set_fn on_set,
                    ui_back_fn back);

void ui_confirm(const char *title, const char *text, const char *yes, ui_back_fn on_yes, ui_back_fn back);

/* Alarms (ui_alarm.c) */
typedef enum { ALARM_MAINT, ALARM_WARN, ALARM_FAIL } ui_alarm_kind_t;
typedef struct {
    uint16_t bit;
    ui_alarm_kind_t kind;
    const char *title;          /* screen header */
    const char *headline;       /* bold line, also the home banner text */
    const char *text;
    const char *action;         /* optional second button */
    ui_back_fn go;
} ui_alarm_info_t;
const ui_alarm_info_t *ui_alarm_top(uint16_t alarms);  /* most important non-fire alarm, NULL if none */

/* Sleep (ui.c): call after something important happened so the screen wakes up */
void ui_wake(void);
