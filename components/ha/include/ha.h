#pragma once
/* Home Assistant over MQTT: discovery, state publishing and commands. Runs only in Wi-Fi mode with a broker set. */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_OFF = 0,         /* no broker configured, or display-only mode */
    HA_CONNECTING,
    HA_CONNECTED,
} ha_state_t;

esp_err_t ha_start(void);
ha_state_t ha_state(void);

#ifdef __cplusplus
}
#endif
