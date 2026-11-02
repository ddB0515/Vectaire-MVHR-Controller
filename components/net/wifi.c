#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "net_priv.h"
#include "settings.h"

static const char *TAG = "net";

#define RECONNECT_DELAY_MS  5000
#define WATCHDOG_MS         10000
#define KICK_AFTER_US       (30 * 1000000LL)        /* offline this long: force a fresh connect, every 30 s */
#define RESTART_AFTER_US    (15 * 60 * 1000000LL)   /* still offline: restart, nobody can reach us otherwise */
#define TEST_TIMEOUT_MS     25000
#define AP_LINGER_MS        15000   /* keep the hotspot after success so the phone sees the result */
#define AP_IDLE_CHECK_MS    10000
#define TRIAL_MS            45000   /* net_change_wifi(): new network must give us an address within this */
#define TRIAL_START_MS      1000

static net_status_t s_status;
static SemaphoreHandle_t s_lock;
static net_listener_t s_listener;
static void *s_listener_arg;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static bool s_wifi_started;
static esp_timer_handle_t s_reconnect_timer, s_test_timer, s_ap_off_timer, s_watchdog_timer, s_ap_idle_timer;
static bool s_ap_open;
static esp_timer_handle_t s_trial_timer, s_trial_start_timer;
static char s_trial_ssid[33], s_trial_pass[65];
static uint32_t s_ap_idle_ms, s_ap_close_after_ms;
static int64_t s_down_since_us, s_last_kick_us;
static bool s_mdns_up;

static void publish(void)
{
    net_status_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_lock);
    if (s_listener) {
        s_listener(&copy, s_listener_arg);
    }
}

static void set_state(net_state_t st, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = st;
    if (err) {
        strlcpy(s_status.last_error, err, sizeof(s_status.last_error));
    }
    xSemaphoreGive(s_lock);
    publish();
}

void net_get_status(net_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void net_set_listener(net_listener_t cb, void *arg)
{
    s_listener_arg = arg;
    s_listener = cb;
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    default:
        return "connection failed";
    }
}

static void mdns_up(void)
{
    if (s_mdns_up) {
        return;
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(settings_device_id());
        mdns_instance_name_set(settings_get()->device_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_up = true;
        ESP_LOGI(TAG, "mDNS: %s.local", settings_device_id());
    }
}

/* After a successful setup: restart, so Home Assistant and everything else that only starts with saved Wi-Fi
   settings comes up in the normal order. */
static void ap_off_cb(void *arg)
{
    if (s_status.state == NET_CONNECTED) {
        ESP_LOGI(TAG, "setup done, restarting into normal operation");
        esp_restart();
    }
}

/* Open hotspot: count the time nobody is joined while it waits for setup; close it when that runs out. */
static void ap_idle_cb(void *arg)
{
    if (s_status.state == NET_CONNECTING || s_status.state == NET_CONNECTED) {
        esp_timer_stop(s_ap_idle_timer);    /* set up: ap_off_cb takes the hotspot down */
        return;
    }
    s_ap_idle_ms = s_status.state == NET_PROVISIONING && !s_status.phone_joined ? s_ap_idle_ms + AP_IDLE_CHECK_MS : 0;
    if (s_ap_idle_ms < s_ap_close_after_ms) {
        return;
    }
    esp_timer_stop(s_ap_idle_timer);
    if (settings_get()->mode == SETTINGS_MODE_WIFI) {
        ESP_LOGI(TAG, "setup hotspot unused, restarting into normal operation");
        esp_restart();
    }
    ESP_LOGI(TAG, "setup hotspot unused, closing it (press the button to open it again)");
    captive_dns_stop();
    esp_wifi_stop();
    set_state(NET_OFF, NULL);
}

static void set_sta_config(const char *ssid, const char *pass);

static void set_wifi_change(const char *fmt, const char *a, const char *b)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.wifi_change, sizeof(s_status.wifi_change), fmt, a, b);
    xSemaphoreGive(s_lock);
}

static void trial_start_cb(void *arg)
{
    ESP_LOGI(TAG, "trying network '%s' (back to '%s' if it fails)", s_trial_ssid, settings_get()->wifi_ssid);
    s_status.wifi_trial = true;
    set_wifi_change("Trying %s%s...", s_trial_ssid, "");
    esp_wifi_disconnect();      /* the disconnect event moves us to NET_CONNECTING */
    set_sta_config(s_trial_ssid, s_trial_pass);
    esp_wifi_connect();
    esp_timer_start_once(s_trial_timer, TRIAL_MS * 1000ULL);
}

/* the new network never gave us an address: back to the saved one */
static void trial_timeout_cb(void *arg)
{
    if (!s_status.wifi_trial) {
        return;
    }
    s_status.wifi_trial = false;
    const settings_t *s = settings_get();
    ESP_LOGW(TAG, "network '%s' failed (%s), back to '%s'", s_trial_ssid, s_status.last_error, s->wifi_ssid);
    char why[64];
    snprintf(why, sizeof(why), "%s", s_status.last_error[0] ? s_status.last_error : "no answer");
    set_wifi_change("Could not connect to %s (%s), kept the old network", s_trial_ssid, why);
    esp_wifi_disconnect();
    set_sta_config(s->wifi_ssid, s->wifi_pass);
    esp_wifi_connect();
    publish();
}

/* called on IP_EVENT_STA_GOT_IP */
static void trial_succeeded(void)
{
    esp_timer_stop(s_trial_timer);
    s_status.wifi_trial = false;
    settings_t s = *settings_get();
    strlcpy(s.wifi_ssid, s_trial_ssid, sizeof(s.wifi_ssid));
    strlcpy(s.wifi_pass, s_trial_pass, sizeof(s.wifi_pass));
    settings_save(&s);
    ESP_LOGI(TAG, "switched to network '%s'", s_trial_ssid);
    set_wifi_change("Connected to %s%s", s_trial_ssid, "");
}

esp_err_t net_change_wifi(const char *ssid, const char *pass)
{
    if (s_status.state != NET_CONNECTED && s_status.state != NET_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status.wifi_trial || !ssid[0] || strlen(ssid) >= sizeof(s_trial_ssid) || strlen(pass) >= sizeof(s_trial_pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_trial_ssid, ssid, sizeof(s_trial_ssid));
    strlcpy(s_trial_pass, pass, sizeof(s_trial_pass));
    return esp_timer_start_once(s_trial_start_timer, TRIAL_START_MS * 1000ULL);
}

bool net_hotspot_open(void)
{
    return s_ap_open;
}

static void test_timeout_cb(void *arg)
{
    if (s_status.state == NET_TESTING) {
        esp_wifi_disconnect();
        set_state(NET_PROVISIONING, s_status.last_error[0] ? s_status.last_error : "no answer from network");
    }
}

static void reconnect_cb(void *arg)
{
    if (s_status.state == NET_CONNECTING) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reconnect: %s", esp_err_to_name(err));
        }
    }
}

/* Last resort for a board without USB: the event-driven reconnect can stall (a failed esp_wifi_connect() raises
   no event), so keep kicking it, and restart if nothing helps. */
static void watchdog_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (s_status.state != NET_CONNECTING) {
        s_down_since_us = 0;
        return;
    }
    if (!s_down_since_us) {
        s_down_since_us = s_last_kick_us = now;
        return;
    }
    if (now - s_down_since_us > RESTART_AFTER_US) {
        ESP_LOGE(TAG, "no Wi-Fi for 15 min, restarting");
        esp_restart();
    }
    if (now - s_down_since_us > KICK_AFTER_US && now - s_last_kick_us > KICK_AFTER_US) {
        s_last_kick_us = now;
        ESP_LOGW(TAG, "still not connected after %lld s, reconnecting", (now - s_down_since_us) / 1000000);
        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", e->reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.ip[0] = 0;
        strlcpy(s_status.last_error, reason_text(e->reason), sizeof(s_status.last_error));
        xSemaphoreGive(s_lock);
        if (s_status.state == NET_CONNECTED) {
            set_state(NET_CONNECTING, NULL);
        }
        if (s_status.state == NET_CONNECTING) {
            esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_MS * 1000ULL);
        } else if (s_status.state == NET_TESTING && e->reason != WIFI_REASON_ASSOC_LEAVE) {
            /* wrong password and network-not-found are final; anything else retries until the timeout */
            if (e->reason == WIFI_REASON_AUTH_FAIL || e->reason == WIFI_REASON_NO_AP_FOUND ||
                e->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                esp_timer_stop(s_test_timer);
                set_state(NET_PROVISIONING, reason_text(e->reason));
            } else {
                esp_wifi_connect();
            }
        }
        publish();
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        s_status.phone_joined = true;
        publish();
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        s_status.phone_joined = false;
        publish();
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = data;
    wifi_ap_record_t ap;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_ip4addr_ntoa(&e->ip_info.ip, s_status.ip, sizeof(s_status.ip));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_status.rssi = ap.rssi;
    }
    s_status.last_error[0] = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "connected to '%s', IP %s", s_status.sta_ssid, s_status.ip);

    if (s_status.state == NET_TESTING) {
        /* credentials work: make them permanent */
        esp_timer_stop(s_test_timer);
        settings_t s = *settings_get();
        s.mode = SETTINGS_MODE_WIFI;
        settings_save(&s);
        esp_timer_start_once(s_ap_off_timer, AP_LINGER_MS * 1000ULL);
    }
    if (s_status.wifi_trial) {
        trial_succeeded();
    }
    set_state(NET_CONNECTED, NULL);
    mdns_up();
    ota_confirm("Wi-Fi connected");
}

static esp_err_t wifi_base_init(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, settings_device_id());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");   /* we keep our own copy */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL), TAG, "ev");

    const esp_timer_create_args_t rc = {.callback = reconnect_cb, .name = "wifi_rc"};
    const esp_timer_create_args_t tt = {.callback = test_timeout_cb, .name = "wifi_test"};
    const esp_timer_create_args_t ao = {.callback = ap_off_cb, .name = "ap_off"};
    esp_timer_create(&rc, &s_reconnect_timer);
    esp_timer_create(&tt, &s_test_timer);
    esp_timer_create(&ao, &s_ap_off_timer);
    const esp_timer_create_args_t wd = {.callback = watchdog_cb, .name = "wifi_wd"};
    esp_timer_create(&wd, &s_watchdog_timer);
    const esp_timer_create_args_t ai = {.callback = ap_idle_cb, .name = "ap_idle"};
    esp_timer_create(&ai, &s_ap_idle_timer);
    const esp_timer_create_args_t ts = {.callback = trial_start_cb, .name = "wifi_trial_go"};
    const esp_timer_create_args_t tt2 = {.callback = trial_timeout_cb, .name = "wifi_trial"};
    esp_timer_create(&ts, &s_trial_start_timer);
    esp_timer_create(&tt2, &s_trial_timer);
    s_wifi_started = true;
    return ESP_OK;
}

static void set_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
}

esp_err_t net_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    ota_init();
    const settings_t *s = settings_get();
    if (s->mode != SETTINGS_MODE_WIFI || !s->wifi_ssid[0]) {
        ota_confirm("no Wi-Fi configured");
        set_state(NET_OFF, NULL);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wifi_base_init(), TAG, "wifi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    set_sta_config(s->wifi_ssid, s->wifi_pass);
    set_state(NET_CONNECTING, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    esp_wifi_connect();
    esp_timer_start_periodic(s_watchdog_timer, WATCHDOG_MS * 1000ULL);
    return portal_start();
}

static esp_err_t start_hotspot(bool open)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_ERROR(wifi_base_init(), TAG, "wifi");
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* random hotspot password, only ever shown on the display (letters/digits that can't be confused) */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ap_open = open;
    for (int i = 0; i < 10; i++) {
        s_status.ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_status.ap_pass[open ? 0 : 10] = 0;
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "Vectaire-%s", settings_device_id() + 9);
    for (char *c = s_status.ap_ssid + 9; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    xSemaphoreGive(s_lock);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_status.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_status.ap_ssid);
    strlcpy((char *)ap.ap.password, s_status.ap_pass, sizeof(ap.ap.password));
    ap.ap.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.pmf_cfg.required = false;

    esp_wifi_stop();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    captive_dns_start(ip.ip.addr);
    ESP_RETURN_ON_ERROR(portal_start(), TAG, "portal");
    ESP_LOGI(TAG, "setup hotspot '%s' up%s", s_status.ap_ssid, open ? " (open)" : "");
    set_state(NET_PROVISIONING, NULL);
    return ESP_OK;
}

esp_err_t net_start_provisioning(void)
{
    return start_hotspot(false);
}

esp_err_t net_start_open_provisioning(uint32_t close_after_s)
{
    if (s_status.state != NET_PROVISIONING && s_status.state != NET_TESTING) {
        ESP_RETURN_ON_ERROR(start_hotspot(true), TAG, "hotspot");
    }
    s_ap_idle_ms = 0;
    s_ap_close_after_ms = close_after_s * 1000;
    esp_timer_stop(s_ap_idle_timer);
    return esp_timer_start_periodic(s_ap_idle_timer, AP_IDLE_CHECK_MS * 1000ULL);
}

esp_err_t net_try_credentials(const char *ssid, const char *pass)
{
    if (s_status.state != NET_PROVISIONING && s_status.state != NET_TESTING) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_wifi_disconnect();
    set_sta_config(ssid, pass);
    s_status.last_error[0] = 0;
    set_state(NET_TESTING, NULL);
    esp_timer_stop(s_test_timer);
    esp_timer_start_once(s_test_timer, TEST_TIMEOUT_MS * 1000ULL);
    return esp_wifi_connect();
}

int net_scan(net_scan_entry_t *out, int max)
{
    const wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {    /* records are sorted by signal, keep the first of each SSID */
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < count && !dup; k++) {
            dup = strcmp(out[k].ssid, (const char *)recs[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        strlcpy(out[count].ssid, (const char *)recs[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out[count].band_5g = recs[i].primary > 14;
        count++;
    }
    free(recs);
    return count;
}

httpd_handle_t net_http_server(void)
{
    return portal_handle();
}
