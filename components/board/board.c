#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_DRAW_LINES      20      /* x2 buffers in internal DMA RAM, keep them small */
#define TP_I2C_PORT         I2C_NUM_0
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0

/* Landscape orientation of the 240x320 panel; adjust if the image is mirrored */
#define LCD_SWAP_XY         true
#define LCD_MIRROR_X        true
#define LCD_MIRROR_Y        false

static lv_display_t *s_disp;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_CLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * LCD_DRAW_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, io_out),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(*io_out, &panel_cfg, panel_out), TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_out), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_out), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel_out, true), TAG, "invert");   /* IPS panel */
    return esp_lcd_panel_disp_on_off(*panel_out, true);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *tp_out)
{
    i2c_master_bus_handle_t i2c_bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TP_I2C_PORT,
        .sda_io_num = BOARD_TP_SDA,
        .scl_io_num = BOARD_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c bus");

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io), TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_V_RES,      /* native portrait 240 x 320 */
        .y_max = BOARD_LCD_H_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = BOARD_TP_INT,
        .flags = {
            .swap_xy = LCD_SWAP_XY,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
    };
    return esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, tp_out);
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t lcd_panel;
    ESP_RETURN_ON_ERROR(lcd_init(&lcd_io, &lcd_panel), TAG, "lcd");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = BOARD_LCD_H_RES * LCD_DRAW_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = LCD_SWAP_XY,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add display");

    esp_lcd_touch_handle_t tp;
    esp_err_t err = touch_init(&tp);
    if (err == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = s_disp, .handle = tp};
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGE(TAG, "add touch failed");
        }
    } else {
        ESP_LOGE(TAG, "touch init failed: %s (display keeps working)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "display %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

lv_display_t *board_display(void)
{
    return s_disp;
}
