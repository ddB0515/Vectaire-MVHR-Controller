#pragma once
/* LVGL user interface. All functions must be called with the LVGL port lock held, except ui_start(). */

#ifdef __cplusplus
extern "C" {
#endif

/* Build the first screen according to the saved mode (first-start choice or home). Takes the lock itself. */
void ui_start(void);

void ui_show_setup_choice(void);
void ui_show_provisioning(void);
void ui_show_home(void);

#ifdef __cplusplus
}
#endif
