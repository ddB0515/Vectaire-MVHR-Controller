idf_build_get_property(headless VECTAIRE_HEADLESS)
set(requires autoclock ha mvhr_model net settings status_led web esp_driver_gpio esp_system)
if(NOT headless)
    list(APPEND requires board ui)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${requires}
)
