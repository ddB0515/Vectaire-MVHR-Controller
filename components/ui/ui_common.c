/* Widget helpers and screen switching. */
#include <stdlib.h>

#include "ui_priv.h"

static const ui_palette_t s_light = {
    .bg = LV_COLOR_MAKE(0xff, 0xff, 0xff), .panel = LV_COLOR_MAKE(0xf3, 0xf6, 0xf8),
    .tile = LV_COLOR_MAKE(0xf7, 0xf9, 0xfb), .line = LV_COLOR_MAKE(0xdf, 0xe5, 0xea),
    .border = LV_COLOR_MAKE(0xc9, 0xd3, 0xdb), .house = LV_COLOR_MAKE(0x9a, 0xa7, 0xb1),
    .text = LV_COLOR_MAKE(0x1d, 0x27, 0x30), .text2 = LV_COLOR_MAKE(0x3d, 0x4a, 0x55),
    .muted = LV_COLOR_MAKE(0x5f, 0x6b, 0x75), .accent = LV_COLOR_MAKE(0x1e, 0x78, 0xc8),
    .accent_dn = LV_COLOR_MAKE(0x15, 0x5a, 0x98), .accent_bg = LV_COLOR_MAKE(0xea, 0xf3, 0xfb),
    .warn = LV_COLOR_MAKE(0xb4, 0x53, 0x09), .warn_bg = LV_COLOR_MAKE(0xff, 0xf1, 0xdc),
    .warn_line = LV_COLOR_MAKE(0xe8, 0xa3, 0x3d), .warn_text = LV_COLOR_MAKE(0x8a, 0x3f, 0x00),
    .hot = LV_COLOR_MAKE(0xd2, 0x55, 0x2b), .fail = LV_COLOR_MAKE(0xc6, 0x28, 0x28),
    .fail_bg = LV_COLOR_MAKE(0xfd, 0xec, 0xea), .fail_text = LV_COLOR_MAKE(0x8e, 0x1b, 0x1b),
    .ok = LV_COLOR_MAKE(0x2e, 0x7d, 0x32),
};

/* same family as the web pages' dark mode */
static const ui_palette_t s_dark = {
    .bg = LV_COLOR_MAKE(0x11, 0x18, 0x1f), .panel = LV_COLOR_MAKE(0x1a, 0x23, 0x2c),
    .tile = LV_COLOR_MAKE(0x1a, 0x23, 0x2c), .line = LV_COLOR_MAKE(0x2b, 0x36, 0x40),
    .border = LV_COLOR_MAKE(0x3a, 0x47, 0x53), .house = LV_COLOR_MAKE(0x5a, 0x6a, 0x77),
    .text = LV_COLOR_MAKE(0xe4, 0xea, 0xef), .text2 = LV_COLOR_MAKE(0xb7, 0xc3, 0xcd),
    .muted = LV_COLOR_MAKE(0x93, 0xa1, 0xad), .accent = LV_COLOR_MAKE(0x2f, 0x86, 0xd2),
    .accent_dn = LV_COLOR_MAKE(0x1f, 0x66, 0xa6), .accent_bg = LV_COLOR_MAKE(0x15, 0x30, 0x48),
    .warn = LV_COLOR_MAKE(0xf0, 0xa0, 0x40), .warn_bg = LV_COLOR_MAKE(0x3a, 0x2a, 0x14),
    .warn_line = LV_COLOR_MAKE(0x8a, 0x5a, 0x14), .warn_text = LV_COLOR_MAKE(0xff, 0xb3, 0x5c),
    .hot = LV_COLOR_MAKE(0xf0, 0x7a, 0x50), .fail = LV_COLOR_MAKE(0xff, 0x6b, 0x5b),
    .fail_bg = LV_COLOR_MAKE(0x3a, 0x1a, 0x1a), .fail_text = LV_COLOR_MAKE(0xff, 0x8a, 0x80),
    .ok = LV_COLOR_MAKE(0x4c, 0xc3, 0x8a),
};

const ui_palette_t *g_ui_pal = &s_light;

static ui_screen_t s_current;
static ui_refresh_fn s_refresh;

static lv_style_t s_btn_primary, s_btn_primary_pr, s_btn_secondary, s_btn_secondary_pr, s_btn_disabled;
static bool s_styles_ready;

static void style_colours(void)
{
    lv_style_set_bg_color(&s_btn_primary, COL_ACCENT);
    lv_style_set_bg_color(&s_btn_primary_pr, COL_ACCENT_DN);
    lv_style_set_bg_color(&s_btn_secondary, COL_BG);
    lv_style_set_border_color(&s_btn_secondary, COL_BORDER);
    lv_style_set_text_color(&s_btn_secondary, COL_TEXT);
    lv_style_set_bg_color(&s_btn_secondary_pr, COL_LINE);
}

static void init_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    lv_style_init(&s_btn_primary);
    lv_style_set_bg_color(&s_btn_primary, COL_ACCENT);
    lv_style_set_bg_opa(&s_btn_primary, LV_OPA_COVER);
    lv_style_set_radius(&s_btn_primary, 8);
    lv_style_set_text_color(&s_btn_primary, COL_WHITE);
    lv_style_set_image_recolor(&s_btn_primary, COL_WHITE);
    lv_style_set_image_recolor_opa(&s_btn_primary, LV_OPA_COVER);
    lv_style_init(&s_btn_primary_pr);
    lv_style_set_bg_color(&s_btn_primary_pr, COL_ACCENT_DN);

    lv_style_init(&s_btn_secondary);
    lv_style_set_bg_color(&s_btn_secondary, COL_BG);
    lv_style_set_bg_opa(&s_btn_secondary, LV_OPA_COVER);
    lv_style_set_border_color(&s_btn_secondary, COL_BORDER);
    lv_style_set_border_width(&s_btn_secondary, 1);
    lv_style_set_radius(&s_btn_secondary, 8);
    lv_style_set_text_color(&s_btn_secondary, COL_TEXT);
    lv_style_init(&s_btn_secondary_pr);
    lv_style_set_bg_color(&s_btn_secondary_pr, COL_LINE);

    lv_style_init(&s_btn_disabled);
    lv_style_set_opa(&s_btn_disabled, LV_OPA_40);
    style_colours();
    s_styles_ready = true;
}

void ui_theme_apply(bool dark)
{
    g_ui_pal = dark ? &s_dark : &s_light;
    if (s_styles_ready) {
        style_colours();
        lv_obj_report_style_change(NULL);
    }
}

bool ui_theme_dark(void)
{
    return g_ui_pal == &s_dark;
}

void ui_load(lv_obj_t *scr, ui_screen_t id, ui_refresh_fn refresh)
{
    lv_obj_t *old = lv_screen_active();
    s_refresh = NULL;
    lv_screen_load(scr);
    if (old && old != scr) {
        lv_obj_delete_async(old);   /* we may be inside one of its event handlers */
    }
    s_current = id;
    s_refresh = refresh;
    if (refresh) {
        mvhr_state_t st;
        mvhr_get_state(&st);
        refresh(&st);
    }
}

ui_screen_t ui_current(void)
{
    return s_current;
}

void ui_refresh_current(const mvhr_state_t *st)
{
    if (s_refresh) {
        s_refresh(st);
    }
}

lv_obj_t *ui_screen(void)
{
    init_styles();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, lv_color_t col)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    lv_obj_set_style_image_recolor(i, col, 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    return i;
}

void ui_style_button(lv_obj_t *b, bool primary)
{
    init_styles();
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, primary ? &s_btn_primary : &s_btn_secondary, 0);
    lv_obj_add_style(b, primary ? &s_btn_primary_pr : &s_btn_secondary_pr, LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_btn_disabled, LV_STATE_DISABLED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(parent);
    ui_style_button(b, primary);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_icon_button(lv_obj_t *parent, const lv_image_dsc_t *img, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(parent);
    ui_style_button(b, false);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    lv_obj_t *i = ui_icon(b, img, COL_ACCENT);
    lv_obj_center(i);
    return b;
}

void ui_go_cb(lv_event_t *e)
{
    ui_back_fn fn = (ui_back_fn)lv_event_get_user_data(e);
    if (fn) {
        fn();
    }
}

lv_obj_t *ui_title_bar(lv_obj_t *scr, const char *title, ui_back_fn back)
{
    lv_obj_t *bar = ui_box(scr);
    lv_obj_set_size(bar, SCR_W, TITLE_H);
    lv_obj_set_style_bg_color(bar, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *t = ui_label(bar, &lv_font_montserrat_18, COL_WHITE, title);
    lv_obj_center(t);
    if (back) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 48, TITLE_H);
        lv_obj_set_style_bg_color(b, COL_ACCENT_DN, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, ui_go_cb, LV_EVENT_CLICKED, (void *)back);
        lv_obj_t *l = ui_label(b, &lv_font_montserrat_18, COL_WHITE, LV_SYMBOL_LEFT);
        lv_obj_center(l);
    }
    return bar;
}

lv_obj_t *ui_bottom_buttons(lv_obj_t *scr, const char *left, lv_event_cb_t left_cb, const char *right,
                            lv_event_cb_t right_cb, lv_color_t right_col)
{
    lv_obj_t *row = ui_box(scr);
    lv_obj_set_size(row, SCR_W, 58);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    if (left) {
        lv_obj_t *b = ui_button(row, left, false, left_cb, NULL);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, LV_PCT(100));
    }
    if (right) {
        lv_obj_t *b = ui_button(row, right, true, right_cb, NULL);
        lv_obj_set_style_bg_color(b, right_col, 0);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, LV_PCT(100));
    }
    return row;
}

void ui_temp_create(ui_temp_t *t, lv_obj_t *parent, const lv_font_t *font, int ring)
{
    t->box = ui_box(parent);
    lv_obj_set_flex_flow(t->box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(t->box, 2, 0);
    t->value = ui_label(t->box, font, COL_TEXT, "--.-");
    t->ring = ui_box(t->box);
    lv_obj_set_size(t->ring, ring, ring);
    lv_obj_set_style_radius(t->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(t->ring, ring >= 8 ? 2 : 1, 0);
    lv_obj_set_style_border_color(t->ring, COL_TEXT, 0);
    lv_obj_set_style_translate_y(t->ring, ring / 2, 0);
}

void ui_temp_set(ui_temp_t *t, bool valid, int16_t x10)
{
    if (!valid) {
        lv_label_set_text(t->value, "--.-");
        return;
    }
    int a = abs(x10);
    lv_label_set_text_fmt(t->value, "%s%d.%d", x10 < 0 ? "-" : "", a / 10, a % 10);
}
