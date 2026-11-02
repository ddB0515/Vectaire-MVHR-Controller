idf_component_register(
    SRCS "vbus_codec.c" "vbus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_http_server
    PRIV_REQUIRES esp_driver_gpio esp_rom esp_timer log
)
