idf_component_register(
    SRCS "mvhr_sim.c" "mvhr.c" "mvhr_decode.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES vectaire_bus esp_timer log
)
