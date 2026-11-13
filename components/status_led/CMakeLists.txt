idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mvhr_model net esp_driver_gpio log espressif__led_strip
)
