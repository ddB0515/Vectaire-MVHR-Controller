idf_component_register(
    SRCS "ui.c" "ui_common.c" "ui_home.c" "ui_settings.c" "ui_editor.c" "ui_alarm.c" "icons.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES autoclock board ha net settings mvhr_model vectaire_bus esp_app_format esp_system log
)
