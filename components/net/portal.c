/* HTTP server: setup page + JSON API while provisioning. Setup endpoints refuse to work outside provisioning. */
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_priv.h"
#include "settings.h"

static const char *TAG = "portal";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");
extern const char ha_html_start[] asm("_binary_ha_html_start");
extern const char ha_html_end[] asm("_binary_ha_html_end");

static httpd_handle_t s_server;
static void (*s_on_server)(httpd_handle_t server);

#define CHUNK_SIZE  1024

esp_err_t net_send_chunked(httpd_req_t *req, const char *data, size_t len)
{
    for (size_t off = 0; off < len; off += CHUNK_SIZE) {
        size_t n = len - off < CHUNK_SIZE ? len - off : CHUNK_SIZE;
        esp_err_t err = httpd_resp_send_chunk(req, data + off, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool provisioning(void)
{
    net_status_t st;
    net_get_status(&st);
    return st.state == NET_PROVISIONING || st.state == NET_TESTING;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t forbidden(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "setup is only available while the setup hotspot is active");
}

static esp_err_t (*s_home_handler)(httpd_req_t *req);

void net_set_home_handler(esp_err_t (*handler)(httpd_req_t *req))
{
    s_home_handler = handler;
}

static esp_err_t root_get(httpd_req_t *req)
{
    if (!provisioning() && s_home_handler) {
        return s_home_handler(req);
    }
    if (!provisioning()) {
        net_status_t st;
        net_get_status(&st);
        char page[384];
        snprintf(page, sizeof(page),
                 "<!doctype html><meta name=viewport content='width=device-width'><title>Vectaire</title>"
                 "<h2>Vectaire MVHR controller</h2><p>%s on %s, IP %s</p><p><a href='/ha'>Home Assistant settings</a></p>",
                 settings_get()->device_name, st.sta_ssid, st.ip);
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req, page);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL terminator; don't send it */
    return net_send_chunked(req, setup_html_start, setup_html_end - setup_html_start - 1);
}

static esp_err_t unauthorized(httpd_req_t *req);

/* while provisioning for the setup page; afterwards with the device password (Wi-Fi change on the home page) */
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!provisioning() && !net_password_ok(req)) {
        return unauthorized(req);
    }
    net_scan_entry_t *list = calloc(20, sizeof(*list));
    if (!list) {
        return httpd_resp_send_500(req);
    }
    int n = net_scan(list, 20);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(o, "secure", list[i].secure);
        cJSON_AddBoolToObject(o, "band5g", list[i].band_5g);
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, arr);
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!provisioning()) {
        return forbidden(req);
    }
    const settings_t *s = settings_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", s->wifi_ssid);
    cJSON_AddStringToObject(o, "mqtt_host", s->mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", s->mqtt_port);
    cJSON_AddStringToObject(o, "mqtt_user", s->mqtt_user);
    cJSON_AddStringToObject(o, "name", s->device_name);
    if (net_hotspot_open()) {
        /* headless: nothing else can show the device password; whoever holds the hotspot sets up the device */
        cJSON_AddStringToObject(o, "device_password", s->ota_password);
    }
    return send_json(req, o);       /* the Wi-Fi and MQTT passwords are never sent back */
}

static void copy_json_str(cJSON *root, const char *key, char *dst, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(dst, v->valuestring, size);
    }
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (!provisioning()) {
        return forbidden(req);
    }
    if (req->content_len <= 0 || req->content_len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
        }
        got += r;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }

    /* store everything except the mode; the mode flips to Wi-Fi only once the connection works */
    settings_t s = *settings_get();
    copy_json_str(root, "ssid", s.wifi_ssid, sizeof(s.wifi_ssid));
    copy_json_str(root, "pass", s.wifi_pass, sizeof(s.wifi_pass));
    copy_json_str(root, "mqtt_host", s.mqtt_host, sizeof(s.mqtt_host));
    copy_json_str(root, "mqtt_user", s.mqtt_user, sizeof(s.mqtt_user));
    const cJSON *mp = cJSON_GetObjectItemCaseSensitive(root, "mqtt_pass");
    if (cJSON_IsString(mp) && mp->valuestring && mp->valuestring[0]) {
        strlcpy(s.mqtt_pass, mp->valuestring, sizeof(s.mqtt_pass));  /* empty = keep the old one */
    }
    char name[sizeof(s.device_name)] = "";
    copy_json_str(root, "name", name, sizeof(name));
    if (name[0]) {
        strlcpy(s.device_name, name, sizeof(s.device_name));
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "mqtt_port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536) {
        s.mqtt_port = (uint16_t)port->valueint;
    }
    cJSON_Delete(root);
    if (!s.wifi_ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing");
    }
    ESP_RETURN_ON_ERROR(settings_save(&s), TAG, "save");
    ESP_LOGI(TAG, "trying network '%s'", s.wifi_ssid);
    net_try_credentials(s.wifi_ssid, s.wifi_pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    static const char *names[] = {"off", "provisioning", "testing", "connecting", "connected"};
    net_status_t st;
    net_get_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", names[st.state]);
    cJSON_AddStringToObject(o, "ssid", st.sta_ssid);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddStringToObject(o, "error", st.last_error);
    cJSON_AddBoolToObject(o, "wifi_trial", st.wifi_trial);
    cJSON_AddStringToObject(o, "wifi_change", st.wifi_change);
    cJSON_AddStringToObject(o, "host", settings_device_id());
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(o, "version", app->version);
    char built[32];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(o, "built", built);
    return send_json(req, o);
}

/* ---- Home Assistant (MQTT) settings, normal operation only, behind the device password */

static esp_err_t ha_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return net_send_chunked(req, ha_html_start, ha_html_end - ha_html_start - 1);
}

static esp_err_t unauthorized(httpd_req_t *req)
{
    vTaskDelay(pdMS_TO_TICKS(1000));    /* slows down guessing */
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "wrong device password");
}

static esp_err_t mqtt_get(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    const settings_t *s = settings_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host", s->mqtt_host);
    cJSON_AddNumberToObject(o, "port", s->mqtt_port);
    cJSON_AddStringToObject(o, "user", s->mqtt_user);
    cJSON_AddStringToObject(o, "name", s->device_name);
    cJSON_AddNumberToObject(o, "interval", s->mqtt_interval_s);
    return send_json(req, o);       /* the password is never sent back */
}

static esp_err_t mqtt_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    if (req->content_len <= 0 || req->content_len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char buf[1025];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    settings_t s = *settings_get();
    s.mqtt_host[0] = 0;
    copy_json_str(root, "host", s.mqtt_host, sizeof(s.mqtt_host));
    s.mqtt_user[0] = 0;
    copy_json_str(root, "user", s.mqtt_user, sizeof(s.mqtt_user));
    const cJSON *mp = cJSON_GetObjectItemCaseSensitive(root, "pass");
    if (cJSON_IsString(mp) && mp->valuestring && mp->valuestring[0]) {
        strlcpy(s.mqtt_pass, mp->valuestring, sizeof(s.mqtt_pass));  /* empty = keep the old one */
    }
    char name[sizeof(s.device_name)] = "";
    copy_json_str(root, "name", name, sizeof(name));
    if (name[0]) {
        strlcpy(s.device_name, name, sizeof(s.device_name));
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536) {
        s.mqtt_port = (uint16_t)port->valueint;
    }
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (cJSON_IsNumber(iv)) {
        if (iv->valueint < SETTINGS_MQTT_INTERVAL_MIN || iv->valueint > SETTINGS_MQTT_INTERVAL_MAX) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "update interval must be 5-3600 s");
        }
        s.mqtt_interval_s = (uint16_t)iv->valueint;
    }
    cJSON_Delete(root);
    ESP_RETURN_ON_ERROR(settings_save(&s), TAG, "save");
    ESP_LOGI(TAG, "MQTT broker set to '%s:%u', restarting", s.mqtt_host, s.mqtt_port);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    net_restart_soon();
    return ESP_OK;
}

/* POST /api/wifi {"ssid","pass"}: switch networks in normal operation (tried first, see net_change_wifi) */
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char buf[513];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    char ssid[33] = "", pass[65] = "";
    copy_json_str(root, "ssid", ssid, sizeof(ssid));
    copy_json_str(root, "pass", pass, sizeof(pass));
    cJSON_Delete(root);
    esp_err_t err = net_change_wifi(ssid, pass);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ssid[0] ? "already switching" : "network name missing");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not connected to Wi-Fi");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/password {"new"}: change the device password. The current one (the generated one until it is first
   changed) goes in the X-OTA-Password header as for every other change. */
static esp_err_t password_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    if (req->content_len <= 0 || req->content_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char buf[257];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    const cJSON *np = cJSON_GetObjectItemCaseSensitive(root, "new");
    bool ok = cJSON_IsString(np) && np->valuestring && settings_password_valid(np->valuestring);
    settings_t s = *settings_get();
    if (ok) {
        strlcpy(s.ota_password, np->valuestring, sizeof(s.ota_password));
    }
    cJSON_Delete(root);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "the new password needs 8-32 characters: letters, digits and symbols, no spaces");
    }
    esp_err_t err = settings_save(&s);
    memset(&s, 0, sizeof(s));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save the password");
    }
    ESP_LOGI(TAG, "device password changed");      /* never the password itself */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Phones probe e.g. /generate_204 or /hotspot-detect.html: send them to the setup page */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (!provisioning()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t portal_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 24;
    cfg.max_open_sockets = 10;      /* LWIP_MAX_SOCKETS is 16; httpd keeps 3 for itself */
    cfg.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/ha", .method = HTTP_GET, .handler = ha_page_get},
        {.uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_get},
        {.uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ota_register(s_server);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    if (s_on_server) {
        s_on_server(s_server);
    }
    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}

void net_on_server_start(void (*cb)(httpd_handle_t server))
{
    s_on_server = cb;
    if (s_server) {
        cb(s_server);
    }
}

httpd_handle_t portal_handle(void)
{
    return s_server;
}
