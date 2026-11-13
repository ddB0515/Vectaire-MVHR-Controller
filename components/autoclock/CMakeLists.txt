idf_component_register(
    SRCS "autoclock.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mvhr_model net settings esp_netif esp_timer log
)
