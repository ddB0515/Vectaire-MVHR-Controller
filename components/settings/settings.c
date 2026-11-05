#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "vectaire";

static settings_t s_settings;
static char s_device_id[16];

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = SETTINGS_MODE_UNSET;
    s->mqtt_port = 1883;
    s->mqtt_interval_s = 30;
    strcpy(s->device_name, "Vectaire MVHR");
    s->backlight_pct = 80;
    s->screen_timeout_s = 300;
    s->dim_timeout_s = 60;
    s->dim_pct = 10;
    s->dst_rule = 4;            /* none: network time keeps standard time all year until a rule is chosen */
}

/* 12 characters without look-alikes (0/O, 1/l/I) */
static void generate_password(char *out, size_t size)
{
    static const char chars[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    size_t n = size - 1 < 12 ? size - 1 : 12;
    for (size_t i = 0; i < n; i++) {
        out[i] = chars[esp_random() % (sizeof(chars) - 1)];
    }
    out[n] = 0;
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        /* keep the default */
    }
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "vectaire-%02x%02x", mac[4], mac[5]);

    defaults(&s_settings);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        uint16_t u16;
        if (nvs_get_u8(h, "mode", &u8) == ESP_OK) {
            s_settings.mode = (settings_mode_t)u8;
        }
        get_str(h, "ssid", s_settings.wifi_ssid, sizeof(s_settings.wifi_ssid));
        get_str(h, "pass", s_settings.wifi_pass, sizeof(s_settings.wifi_pass));
        get_str(h, "mqtt_host", s_settings.mqtt_host, sizeof(s_settings.mqtt_host));
        get_str(h, "mqtt_user", s_settings.mqtt_user, sizeof(s_settings.mqtt_user));
        get_str(h, "mqtt_pass", s_settings.mqtt_pass, sizeof(s_settings.mqtt_pass));
        get_str(h, "name", s_settings.device_name, sizeof(s_settings.device_name));
        get_str(h, "ota_pass", s_settings.ota_password, sizeof(s_settings.ota_password));
        if (nvs_get_u16(h, "mqtt_port", &u16) == ESP_OK) {
            s_settings.mqtt_port = u16;
        }
        if (nvs_get_u16(h, "mqtt_iv", &u16) == ESP_OK && u16 >= SETTINGS_MQTT_INTERVAL_MIN &&
            u16 <= SETTINGS_MQTT_INTERVAL_MAX) {
            s_settings.mqtt_interval_s = u16;
        }
        if (nvs_get_u8(h, "backlight", &u8) == ESP_OK) {
            s_settings.backlight_pct = u8;
        }
        if (nvs_get_u16(h, "timeout", &u16) == ESP_OK) {
            s_settings.screen_timeout_s = u16;
        }
        if (nvs_get_u16(h, "dim", &u16) == ESP_OK) {
            s_settings.dim_timeout_s = u16;
        }
        if (nvs_get_u8(h, "dim_pct", &u8) == ESP_OK && u8 >= 1 && u8 <= 100) {
            s_settings.dim_pct = u8;
        }
        int16_t i16;
        if (nvs_get_i16(h, "out_off", &i16) == ESP_OK && i16 >= -100 && i16 <= 100) {
            s_settings.outdoor_offset_x10 = i16;
        }
        if (nvs_get_u8(h, "ntp_on", &u8) == ESP_OK) {
            s_settings.ntp_on = u8;
        }
        if (nvs_get_i16(h, "tz_x10", &i16) == ESP_OK && i16 >= -120 && i16 <= 140) {
            s_settings.tz_std_x10 = i16;
        }
        if (nvs_get_u8(h, "dst_rule", &u8) == ESP_OK && u8 <= 4) {
            s_settings.dst_rule = u8;
        }
        if (nvs_get_u8(h, "dark", &u8) == ESP_OK) {
            s_settings.dark_theme = u8;
        }
        if (nvs_get_u8(h, "sim", &u8) == ESP_OK) {
            s_settings.unit_simulator = u8;
        }
        nvs_close(h);
    }
    if (!s_settings.ota_password[0]) {
        generate_password(s_settings.ota_password, sizeof(s_settings.ota_password));
        ESP_RETURN_ON_ERROR(settings_save(&s_settings), TAG, "save OTA password");
    }
    ESP_LOGI(TAG, "id %s, mode %d, ssid '%s', mqtt '%s:%u', sim %d", s_device_id, s_settings.mode,
             s_settings.wifi_ssid, s_settings.mqtt_host, s_settings.mqtt_port, s_settings.unit_simulator);
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = ESP_OK;
    err |= nvs_set_u8(h, "mode", (uint8_t)s->mode);
    err |= nvs_set_str(h, "ssid", s->wifi_ssid);
    err |= nvs_set_str(h, "pass", s->wifi_pass);
    err |= nvs_set_str(h, "mqtt_host", s->mqtt_host);
    err |= nvs_set_u16(h, "mqtt_port", s->mqtt_port);
    err |= nvs_set_str(h, "mqtt_user", s->mqtt_user);
    err |= nvs_set_str(h, "mqtt_pass", s->mqtt_pass);
    err |= nvs_set_str(h, "name", s->device_name);
    err |= nvs_set_u16(h, "mqtt_iv", s->mqtt_interval_s);
    err |= nvs_set_u8(h, "backlight", s->backlight_pct);
    err |= nvs_set_u16(h, "timeout", s->screen_timeout_s);
    err |= nvs_set_u16(h, "dim", s->dim_timeout_s);
    err |= nvs_set_u8(h, "dim_pct", s->dim_pct);
    err |= nvs_set_i16(h, "out_off", s->outdoor_offset_x10);
    err |= nvs_set_u8(h, "ntp_on", s->ntp_on);
    err |= nvs_set_i16(h, "tz_x10", s->tz_std_x10);
    err |= nvs_set_u8(h, "dst_rule", s->dst_rule);
    err |= nvs_set_u8(h, "sim", s->unit_simulator);
    err |= nvs_set_u8(h, "dark", s->dark_theme);
    err |= nvs_set_str(h, "ota_pass", s->ota_password);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "write");
    if (s != &s_settings) {
        s_settings = *s;
    }
    return ESP_OK;
}

esp_err_t settings_reset_network(void)
{
    settings_t s = s_settings;
    s.mode = SETTINGS_MODE_UNSET;
    s.wifi_ssid[0] = s.wifi_pass[0] = 0;
    return settings_save(&s);
}

const char *settings_device_id(void)
{
    return s_device_id;
}
