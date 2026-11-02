#pragma once
/* Persistent device settings (NVS namespace "vectaire"). */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SETTINGS_MODE_UNSET = 0,        /* first start: ask the user */
    SETTINGS_MODE_WIFI = 1,         /* Wi-Fi + Home Assistant */
    SETTINGS_MODE_DISPLAY_ONLY = 2,
} settings_mode_t;

#define SETTINGS_MQTT_INTERVAL_MIN  5
#define SETTINGS_MQTT_INTERVAL_MAX  3600

typedef struct {
    settings_mode_t mode;
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[33];
    char mqtt_pass[65];
    char device_name[33];           /* shown in HA, default "Vectaire MVHR" */
    uint16_t mqtt_interval_s;       /* HA sensor readings every this long (changes go out at once) */
    uint8_t backlight_pct;
    uint16_t dim_timeout_s;         /* no touch for this long: dim the backlight; 0 = never */
    uint16_t screen_timeout_s;      /* no touch for this long: backlight off; 0 = never */
    bool dark_theme;                /* display colours */
    bool unit_simulator;            /* run against the built-in simulated unit */
    char ota_password[17];          /* for firmware uploads over Wi-Fi; generated on first start */
} settings_t;

esp_err_t settings_init(void);      /* nvs_flash_init + load */
const settings_t *settings_get(void);
esp_err_t settings_save(const settings_t *s);

/* Forget Wi-Fi/MQTT and return to the first-start choice. */
esp_err_t settings_reset_network(void);

/* "vectaire-1a2b" style id from the MAC, used for hostname, hotspot and MQTT topics */
const char *settings_device_id(void);

#ifdef __cplusplus
}
#endif
