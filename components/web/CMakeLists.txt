# cJSON left ESP-IDF in 6.0 (managed components, see idf_component.yml)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(json espressif__cjson)
else()
    set(json json)
endif()

idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES net ha mvhr_model settings vectaire_bus ${json} esp_app_format espcoredump heap esp_timer log
    EMBED_TXTFILES "www/home.html" "www/diag.html"
)
