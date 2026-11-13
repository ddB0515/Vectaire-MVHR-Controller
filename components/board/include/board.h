#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NiceMCU C5E-DEV 2.8IPS pin map */
#define BOARD_LCD_DC        4
#define BOARD_LCD_CS        5
#define BOARD_LCD_CLK       6
#define BOARD_LCD_MOSI      7
#define BOARD_LCD_BL        25
#define BOARD_TP_SDA        24
#define BOARD_TP_SCL        23
#define BOARD_TP_INT        1
#define BOARD_LED_WS2812    27
#define BOARD_BUZZER        26
/* button (28) and MVHR bus (TX 2, RX 3 on the 4-pin header): CONFIG_VECTAIRE_* in main/Kconfig.projbuild */

/* Panel is 240x320 portrait; the UI runs landscape */
#define BOARD_LCD_H_RES     320
#define BOARD_LCD_V_RES     240

/* Initialise display, touch, backlight and the LVGL port. Backlight stays off until set. */
esp_err_t board_init(void);

lv_display_t *board_display(void);

/* Backlight 0..100 % */
void board_backlight_set(uint8_t percent);

#ifdef __cplusplus
}
#endif
