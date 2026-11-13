# cJSON left ESP-IDF in 6.0 (managed components, see idf_component.yml)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(json espressif__cjson)
else()
    set(json json)
endif()

idf_component_register(
    SRCS "ota.c" "wifi.c" "portal.c" "captive_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update esp_app_format bootloader_support esp_wifi esp_netif esp_event esp_timer lwip ${json} settings log espressif__mdns
    EMBED_TXTFILES "www/setup.html" "www/ha.html"
)
