/* Generic editors: one value with arrows, several spin fields (time, date), and a yes/no confirmation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

/* ---------------------------------------------------------------- single value */

static struct {
    int value, min, max, step, repeats;
    bool tenths;                /* value is in tenths and shown as +1.3 (no fmt) */
    const char *fmt;
    ui_value_set_fn on_set;
    ui_back_fn back;
    lv_obj_t *label;
} s_ed;

static void value_show(void)
{
    if (s_ed.tenths) {
        lv_label_set_text_fmt(s_ed.label, "%c%d.%d", s_ed.value < 0 ? '-' : '+', abs(s_ed.value) / 10,
                              abs(s_ed.value) % 10);
    } else {
        lv_label_set_text_fmt(s_ed.label, s_ed.fmt, s_ed.value);
    }
}

static void value_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_ed.repeats = 0;
    }
    int mult = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT && ++s_ed.repeats > 10 ? 5 : 1;
    int v = s_ed.value + dir * s_ed.step * mult;
    s_ed.value = v < s_ed.min ? s_ed.min : v > s_ed.max ? s_ed.max : v;
    value_show();
}

static void value_set_cb(lv_event_t *e)
{
    s_ed.on_set(s_ed.value);
    s_ed.back();
}

static void value_cancel_cb(lv_event_t *e)
{
    s_ed.back();
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int dir, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    ui_style_button(b, false);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dir);
    lv_obj_center(ui_label(b, &lv_font_montserrat_22, COL_ACCENT, sym));
    return b;
}

void ui_edit_value(const char *title, const char *prompt, int value, int min, int max, int step, const char *fmt,
                   ui_value_set_fn on_set, ui_back_fn back)
{
    s_ed.value = value < min ? min : value > max ? max : value;
    s_ed.min = min;
    s_ed.max = max;
    s_ed.step = step;
    s_ed.fmt = fmt;
    s_ed.tenths = false;
    s_ed.on_set = on_set;
    s_ed.back = back;

    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, title, NULL);
    lv_obj_t *p = ui_label(scr, &lv_font_montserrat_14, COL_TEXT2, prompt);
    lv_obj_set_width(p, SCR_W - 20);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, TITLE_H + 12);

    lv_obj_t *row = ui_box(scr);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, TITLE_H + 48);
    round_button(row, LV_SYMBOL_LEFT, -1, value_step_cb);
    lv_obj_t *box = ui_box(row);
    lv_obj_set_size(box, 136, 60);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, COL_ACCENT, 0);
    lv_obj_set_style_radius(box, 8, 0);
    s_ed.label = ui_label(box, &lv_font_montserrat_32, COL_TEXT, "");
    lv_obj_center(s_ed.label);
    round_button(row, LV_SYMBOL_RIGHT, 1, value_step_cb);

    lv_obj_t *hint = ui_label(scr, &lv_font_montserrat_12, COL_MUTED, "Hold an arrow to step faster");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, TITLE_H + 118);

    ui_bottom_buttons(scr, "Cancel", value_cancel_cb, "Set", value_set_cb, COL_ACCENT);
    value_show();
    ui_load(scr, SCR_EDIT, NULL);
}

/* Same editor for a value in tenths (e.g. +1.3), step in tenths */
void ui_edit_tenths(const char *title, const char *prompt, int value_x10, int min_x10, int max_x10, int step_x10,
                    ui_value_set_fn on_set, ui_back_fn back)
{
    ui_edit_value(title, prompt, value_x10, min_x10, max_x10, step_x10, "", on_set, back);
    s_ed.tenths = true;
    value_show();
}

/* ---------------------------------------------------------------- several fields */

static struct {
    ui_field_t f[UI_MAX_FIELDS];
    int n, repeats;
    ui_fields_set_fn on_set;
    ui_back_fn back;
    lv_obj_t *label[UI_MAX_FIELDS];
} s_fe;

static void field_show(int i)
{
    lv_label_set_text_fmt(s_fe.label[i], s_fe.f[i].fmt, s_fe.f[i].value);
}

/* user data: field index * 2 + (1 = up) */
static void field_step_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int i = code / 2, dir = code & 1 ? 1 : -1;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_fe.repeats = 0;
    }
    int mult = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT && ++s_fe.repeats > 10 ? 2 : 1;
    ui_field_t *f = &s_fe.f[i];
    int v = f->value + dir * f->step * mult;
    /* wrap around like a clock */
    if (v > f->max) {
        v = f->min;
    } else if (v < f->min) {
        v = f->max - (f->max - f->min) % f->step;
    }
    f->value = v;
    field_show(i);
}

static void fields_set_cb(lv_event_t *e)
{
    int v[UI_MAX_FIELDS];
    for (int i = 0; i < s_fe.n; i++) {
        v[i] = s_fe.f[i].value;
    }
    s_fe.on_set(v);
    s_fe.back();
}

static void fields_cancel_cb(lv_event_t *e)
{
    s_fe.back();
}

static lv_obj_t *arrow_button(lv_obj_t *parent, int w, const char *sym, int code)
{
    lv_obj_t *b = lv_button_create(parent);
    ui_style_button(b, false);
    lv_obj_set_size(b, w, 38);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_add_event_cb(b, field_step_cb, LV_EVENT_PRESSED, (void *)(intptr_t)code);
    lv_obj_add_event_cb(b, field_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)code);
    lv_obj_center(ui_label(b, &lv_font_montserrat_18, COL_ACCENT, sym));
    return b;
}

void ui_edit_fields(const char *title, const ui_field_t *fields, int n, const char *sep, ui_fields_set_fn on_set,
                    ui_back_fn back)
{
    s_fe.n = n > UI_MAX_FIELDS ? UI_MAX_FIELDS : n;
    memcpy(s_fe.f, fields, s_fe.n * sizeof(ui_field_t));
    s_fe.on_set = on_set;
    s_fe.back = back;

    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, title, NULL);
    lv_obj_t *row = ui_box(scr);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, TITLE_H + 8);
    for (int i = 0; i < s_fe.n; i++) {
        if (i) {
            ui_label(row, &lv_font_montserrat_28, COL_MUTED, sep);
        }
        int w = s_fe.f[i].max >= 1000 ? 100 : 70;
        lv_obj_t *col = ui_box(row);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 4, 0);
        arrow_button(col, w, LV_SYMBOL_UP, i * 2 + 1);
        lv_obj_t *box = ui_box(col);
        lv_obj_set_size(box, w, 46);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, COL_ACCENT, 0);
        lv_obj_set_style_radius(box, 8, 0);
        s_fe.label[i] = ui_label(box, &lv_font_montserrat_28, COL_TEXT, "");
        lv_obj_center(s_fe.label[i]);
        arrow_button(col, w, LV_SYMBOL_DOWN, i * 2);
        field_show(i);
    }
    ui_bottom_buttons(scr, "Cancel", fields_cancel_cb, "Set", fields_set_cb, COL_ACCENT);
    ui_load(scr, SCR_EDIT, NULL);
}

/* ---------------------------------------------------------------- confirmation */

static ui_back_fn s_yes, s_no;

static void yes_cb(lv_event_t *e)
{
    s_yes();
}

static void no_cb(lv_event_t *e)
{
    s_no();
}

void ui_confirm(const char *title, const char *text, const char *yes, ui_back_fn on_yes, ui_back_fn back)
{
    s_yes = on_yes;
    s_no = back;
    lv_obj_t *scr = ui_screen();
    ui_title_bar(scr, title, NULL);
    lv_obj_t *icon = ui_icon(scr, &ic_alert_32, COL_WARN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, TITLE_H + 18);
    lv_obj_t *t = ui_label(scr, &lv_font_montserrat_14, COL_TEXT, text);
    lv_obj_set_width(t, SCR_W - 24);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, TITLE_H + 62);
    ui_bottom_buttons(scr, "Cancel", no_cb, yes, yes_cb, COL_WARN);
    ui_load(scr, SCR_EDIT, NULL);
}
