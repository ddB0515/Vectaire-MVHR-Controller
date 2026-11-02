# MQTT and cJSON left ESP-IDF in 6.0 (managed components, see idf_component.yml)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(json espressif__cjson)
    set(mqtt espressif__mqtt)
else()
    set(json json)
    set(mqtt mqtt)
endif()

idf_component_register(
    SRCS "ha.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${mqtt} ${json} mvhr_model net settings esp_app_format log
)
