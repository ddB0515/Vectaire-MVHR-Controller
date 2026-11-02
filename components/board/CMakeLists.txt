idf_component_register(
    SRCS "board.c" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
             lvgl__lvgl espressif__esp_lvgl_port
    PRIV_REQUIRES esp_psram heap spi_flash espressif__esp_lcd_touch_cst816s
    WHOLE_ARCHIVE   # lv_mem_psram.c provides symbols LVGL needs; LVGL links after us
)
