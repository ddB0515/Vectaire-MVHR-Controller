/*
 * Home Assistant via MQTT discovery. One retained JSON state message, one config per entity, commands on
 * vectaire/<id>/<entity>/set. Availability needs both the controller (LWT) and the unit to be online.
 */
#include "ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mvhr.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "ha";

#define PREFIX          "homeassistant"
#define COALESCE_MS     2000    /* gather changes before publishing */

typedef enum { E_SENSOR, E_BINARY, E_SELECT, E_SWITCH, E_NUMBER, E_BUTTON } kind_t;
typedef enum { SC_NONE, SC_MEASUREMENT, SC_TOTAL_INCREASING } state_class_t;

typedef struct {
    kind_t kind;
    const char *obj;            /* object id, JSON key and command topic part */
    const char *name;
    const char *device_class;
    const char *unit;
    const char *icon;
    const char *category;       /* NULL, "config" or "diagnostic" */
    state_class_t state_class;
    int min, max, step;         /* number */
    const char *options;        /* select, '|' separated */
} entity_t;

#define DEG_C "\xc2\xb0" "C"

static const entity_t s_entities[] = {
    {.kind = E_SENSOR, .obj = "t_in", .name = "Indoor temperature", .device_class = "temperature", .unit = DEG_C,
     .state_class = SC_MEASUREMENT},
    {.kind = E_SENSOR, .obj = "t_out", .name = "Outdoor temperature", .device_class = "temperature", .unit = DEG_C,
     .state_class = SC_MEASUREMENT},
    {.kind = E_SENSOR, .obj = "humidity", .name = "Indoor humidity", .device_class = "humidity", .unit = "%",
     .state_class = SC_MEASUREMENT},
    {.kind = E_SENSOR, .obj = "fan_intake", .name = "Intake fan speed", .unit = "%", .icon = "mdi:fan",
     .state_class = SC_MEASUREMENT},
    {.kind = E_SENSOR, .obj = "fan_exhaust", .name = "Exhaust fan speed", .unit = "%", .icon = "mdi:fan",
     .state_class = SC_MEASUREMENT},
    {.kind = E_SELECT, .obj = "fan", .name = "Fan speed", .icon = "mdi:fan", .options = "1|2|3"},
    {.kind = E_SELECT, .obj = "bypass", .name = "Bypass", .icon = "mdi:swap-horizontal",
     .options = "Auto|Heat recovery|Bypass"},
    {.kind = E_SWITCH, .obj = "holiday", .name = "Holiday mode", .icon = "mdi:beach"},
    {.kind = E_SWITCH, .obj = "night", .name = "Night mode", .icon = "mdi:weather-night", .category = "config"},
    {.kind = E_NUMBER, .obj = "rh_set", .name = "Humidity setpoint", .unit = "%", .icon = "mdi:water-percent",
     .category = "config", .min = 35, .max = 99, .step = 1},
    {.kind = E_NUMBER, .obj = "boost_delay", .name = "Boost delay", .unit = "s", .icon = "mdi:timer-sand",
     .category = "config", .min = 0, .max = 300, .step = 5},
    {.kind = E_NUMBER, .obj = "overrun", .name = "Overrun", .unit = "min", .icon = "mdi:timer-outline",
     .category = "config", .min = 0, .max = 90, .step = 1},
    {.kind = E_SENSOR, .obj = "filter", .name = "Filter saturation", .unit = "%", .icon = "mdi:air-filter",
     .state_class = SC_MEASUREMENT},
    {.kind = E_BUTTON, .obj = "reset_filter", .name = "Reset filter counter", .icon = "mdi:restore",
     .category = "config"},
    {.kind = E_SENSOR, .obj = "function", .name = "Function", .icon = "mdi:fan-auto"},
    {.kind = E_SENSOR, .obj = "mode", .name = "Mode", .icon = "mdi:swap-horizontal-variant"},
    {.kind = E_SENSOR, .obj = "boost", .name = "Boost", .icon = "mdi:fan-chevron-up"},
    {.kind = E_SENSOR, .obj = "alarm", .name = "Alarm", .icon = "mdi:alert-circle-outline"},
    {.kind = E_BINARY, .obj = "problem", .name = "Problem", .device_class = "problem"},
    {.kind = E_BINARY, .obj = "fire", .name = "Fire alarm", .device_class = "safety"},
    {.kind = E_BINARY, .obj = "frost", .name = "Frost protection", .device_class = "cold"},
    {.kind = E_SENSOR, .obj = "hours", .name = "Hours of service", .unit = "h", .icon = "mdi:clock-outline",
     .category = "diagnostic", .state_class = SC_TOTAL_INCREASING},
    {.kind = E_SENSOR, .obj = "outages", .name = "Power outages", .icon = "mdi:power-plug-off",
     .category = "diagnostic", .state_class = SC_TOTAL_INCREASING},
    {.kind = E_SENSOR, .obj = "rssi", .name = "Wi-Fi signal", .device_class = "signal_strength", .unit = "dBm",
     .category = "diagnostic", .state_class = SC_MEASUREMENT},
};

static esp_mqtt_client_handle_t s_client;
static volatile ha_state_t s_state;
static volatile bool s_need_discovery;
static TaskHandle_t s_task;
static char s_base[48], s_avail[64], s_unit_avail[64], s_state_topic[64];

ha_state_t ha_state(void)
{
    return s_state;
}

static void kick(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* qos 1 for the rare discovery messages; the frequent state goes qos 0 (no outbox copy, it repeats anyway) */
static void publish_json(const char *topic, cJSON *o, int qos, bool retain)
{
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) {
        esp_mqtt_client_publish(s_client, topic, txt, 0, qos, retain);
        cJSON_free(txt);
    }
}

static void publish_discovery(void)
{
    static const char *components[] = {"sensor", "binary_sensor", "select", "switch", "number", "button"};
    static const char *state_classes[] = {NULL, "measurement", "total_increasing"};
    const char *id = settings_device_id();
    char buf[128];

    for (size_t i = 0; i < sizeof(s_entities) / sizeof(s_entities[0]); i++) {
        const entity_t *e = &s_entities[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        snprintf(buf, sizeof(buf), "%s_%s", id, e->obj);
        cJSON_AddStringToObject(o, "unique_id", buf);
        if (e->kind != E_BUTTON) {
            cJSON_AddStringToObject(o, "state_topic", s_state_topic);
            snprintf(buf, sizeof(buf), "{{ value_json.%s }}", e->obj);
            cJSON_AddStringToObject(o, "value_template", buf);
        }
        if (e->kind == E_SELECT || e->kind == E_SWITCH || e->kind == E_NUMBER || e->kind == E_BUTTON) {
            snprintf(buf, sizeof(buf), "%s/%s/set", s_base, e->obj);
            cJSON_AddStringToObject(o, "command_topic", buf);
        }
        if (e->device_class) {
            cJSON_AddStringToObject(o, "device_class", e->device_class);
        }
        if (e->unit) {
            cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
        }
        if (e->icon) {
            cJSON_AddStringToObject(o, "icon", e->icon);
        }
        if (e->category) {
            cJSON_AddStringToObject(o, "entity_category", e->category);
        }
        if (state_classes[e->state_class]) {
            cJSON_AddStringToObject(o, "state_class", state_classes[e->state_class]);
        }
        if (e->kind == E_NUMBER) {
            cJSON_AddNumberToObject(o, "min", e->min);
            cJSON_AddNumberToObject(o, "max", e->max);
            cJSON_AddNumberToObject(o, "step", e->step);
            cJSON_AddStringToObject(o, "mode", "box");
        }
        if (e->kind == E_SELECT) {
            cJSON *opts = cJSON_AddArrayToObject(o, "options");
            char tmp[64];
            strlcpy(tmp, e->options, sizeof(tmp));
            for (char *save, *tok = strtok_r(tmp, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
                cJSON_AddItemToArray(opts, cJSON_CreateString(tok));
            }
        }
        if (e->kind == E_BUTTON) {
            cJSON_AddStringToObject(o, "payload_press", "PRESS");
        }

        cJSON *avail = cJSON_AddArrayToObject(o, "availability");
        cJSON *a1 = cJSON_CreateObject(), *a2 = cJSON_CreateObject();
        cJSON_AddStringToObject(a1, "topic", s_avail);
        cJSON_AddStringToObject(a2, "topic", s_unit_avail);
        cJSON_AddItemToArray(avail, a1);
        cJSON_AddItemToArray(avail, a2);
        cJSON_AddStringToObject(o, "availability_mode", "all");

        cJSON *dev = cJSON_AddObjectToObject(o, "device");
        cJSON_AddItemToObject(dev, "identifiers", cJSON_CreateStringArray(&id, 1));
        cJSON_AddStringToObject(dev, "name", settings_get()->device_name);
        cJSON_AddStringToObject(dev, "manufacturer", "Vectaire");
        cJSON_AddStringToObject(dev, "model", "MVHR (replacement panel)");
        cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
        snprintf(buf, sizeof(buf), "http://%s.local/", id);
        cJSON_AddStringToObject(dev, "configuration_url", buf);

        char topic[96];
        snprintf(topic, sizeof(topic), PREFIX "/%s/%s/%s/config", components[e->kind], id, e->obj);
        publish_json(topic, o, 1, true);
    }
    /* entities of older firmware: an empty retained config removes them from Home Assistant */
    static const char *const removed[] = {"sensor/%s/rh_in", "sensor/%s/rh_out"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        char topic[96], part[64];
        snprintf(part, sizeof(part), removed[i], id);
        snprintf(topic, sizeof(topic), PREFIX "/%s/config", part);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, true);
    }
    ESP_LOGI(TAG, "discovery sent (%u entities)", (unsigned)(sizeof(s_entities) / sizeof(s_entities[0])));
}

static const char *on_off(bool v)
{
    return v ? "ON" : "OFF";
}

static void publish_state(void)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    esp_mqtt_client_publish(s_client, s_unit_avail, st.online ? "online" : "offline", 0, 0, 1);
    if (!(st.valid & MVHR_G_STATUS)) {
        return;
    }
    static const char *bypass[] = {"Heat recovery", "Bypass", "Bypass"};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t_in", st.temp_extract_x10 / 10.0);
    cJSON_AddNumberToObject(o, "t_out", st.temp_supply_x10 / 10.0);
    /* the reading as the unit reports it, also below 35 % where the panel only shows "low" */
    if (st.humidity_x10 <= MVHR_RH_VALID_MAX_X10) {
        cJSON_AddNumberToObject(o, "humidity", st.humidity_x10 / 10.0);
    } else {
        cJSON_AddNullToObject(o, "humidity");
    }
    cJSON_AddNumberToObject(o, "fan_intake", st.fan_a_pct);
    cJSON_AddNumberToObject(o, "fan_exhaust", st.fan_b_pct);
    char fan[4];
    snprintf(fan, sizeof(fan), "%u", st.fan_speed);
    cJSON_AddStringToObject(o, "fan", fan);
    cJSON_AddStringToObject(o, "bypass", st.bypass_override == MVHR_BYPASS_AUTO ? "Auto"
                                         : st.bypass_override <= 2 ? bypass[st.bypass_override] : "Auto");
    cJSON_AddStringToObject(o, "holiday", on_off(st.holiday));
    if (st.night_known) {
        cJSON_AddStringToObject(o, "night", on_off(st.night_enabled));
    } else {
        cJSON_AddNullToObject(o, "night");
    }
    if (st.valid & MVHR_G_SETTINGS) {
        cJSON_AddNumberToObject(o, "rh_set", st.humidity_setpoint);
        cJSON_AddNumberToObject(o, "boost_delay", st.boost_delay_s);
        cJSON_AddNumberToObject(o, "overrun", st.overrun_s / 60);
    }
    if (st.valid & MVHR_G_COUNTERS) {
        cJSON_AddNumberToObject(o, "filter", st.filter_pct);
        cJSON_AddNumberToObject(o, "hours", st.hours);
        cJSON_AddNumberToObject(o, "outages", st.power_outages);
    }
    const char *fn = mvhr_function_text(&st);
    cJSON_AddStringToObject(o, "function", fn ? fn : "Normal");
    cJSON_AddStringToObject(o, "mode", mvhr_airflow_text(&st));
    cJSON_AddStringToObject(o, "boost", mvhr_boost_text(&st));
    /* not "None": Home Assistant reads a template result of "None" as no value and shows "Unknown" */
    cJSON_AddStringToObject(o, "alarm", st.alarms ? mvhr_alarm_text(st.alarms) : "No alarm");
    cJSON_AddStringToObject(o, "problem", on_off(st.alarms & ~MVHR_ALARM_FIRE));
    cJSON_AddStringToObject(o, "fire", on_off(st.alarms & MVHR_ALARM_FIRE));
    cJSON_AddStringToObject(o, "frost", on_off(st.frost));
    net_status_t n;
    net_get_status(&n);
    cJSON_AddNumberToObject(o, "rssi", n.rssi);
    publish_json(s_state_topic, o, 0, true);
}

/* "vectaire/<id>/<obj>/set" with a short text payload */
static void handle_command(const char *obj, const char *val)
{
    mvhr_state_t st;
    mvhr_get_state(&st);
    bool on = strcmp(val, "ON") == 0;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (!strcmp(obj, "fan")) {
        err = mvhr_set_fan_speed((uint8_t)atoi(val));
    } else if (!strcmp(obj, "bypass")) {
        err = mvhr_set_bypass(!strcmp(val, "Heat recovery") ? 0 : !strcmp(val, "Bypass") ? 1 : MVHR_BYPASS_AUTO);
    } else if (!strcmp(obj, "holiday")) {
        err = mvhr_set_holiday(on);
    } else if (!strcmp(obj, "night")) {
        err = st.night_known ? mvhr_set_night_inhibition(on, st.night_start_min, st.night_end_min)
                             : ESP_ERR_INVALID_STATE;
    } else if (!strcmp(obj, "rh_set")) {
        err = mvhr_set_humidity_setpoint((uint8_t)atoi(val));
    } else if (!strcmp(obj, "boost_delay")) {
        err = mvhr_set_boost_delay((uint16_t)atoi(val));
    } else if (!strcmp(obj, "overrun")) {
        err = mvhr_set_overrun((uint16_t)(atoi(val) * 60));
    } else if (!strcmp(obj, "reset_filter") && !strcmp(val, "PRESS")) {
        err = mvhr_reset_filter();
    }
    ESP_LOGI(TAG, "command %s = %s: %s", obj, val, esp_err_to_name(err));
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");
        s_state = HA_CONNECTED;
        char topic[80];
        snprintf(topic, sizeof(topic), "%s/+/set", s_base);
        esp_mqtt_client_subscribe(s_client, topic, 1);
        esp_mqtt_client_subscribe(s_client, PREFIX "/status", 1);
        s_need_discovery = true;
        kick();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_state == HA_CONNECTED) {
            ESP_LOGW(TAG, "disconnected from broker");
        }
        s_state = HA_CONNECTING;
        break;
    case MQTT_EVENT_DATA: {
        char topic[96], val[32];
        if (ev->topic_len >= (int)sizeof(topic) || ev->data_len >= (int)sizeof(val) || ev->current_data_offset) {
            break;
        }
        memcpy(topic, ev->topic, ev->topic_len);
        topic[ev->topic_len] = 0;
        memcpy(val, ev->data, ev->data_len);
        val[ev->data_len] = 0;
        if (!strcmp(topic, PREFIX "/status")) {
            if (!strcmp(val, "online")) {   /* HA restarted: announce everything again */
                s_need_discovery = true;
                kick();
            }
            break;
        }
        size_t bl = strlen(s_base);
        char *obj = topic + bl + 1, *slash;
        if (strncmp(topic, s_base, bl) || topic[bl] != '/' || !(slash = strchr(obj, '/')) || strcmp(slash, "/set")) {
            break;
        }
        *slash = 0;
        handle_command(obj, val);
        kick();
        break;
    }
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "broker refused the connection (code %d): check user and password",
                     ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static void on_unit_change(uint32_t changed, void *arg)
{
    kick();
}

/* What someone switched or should see at once; the sensor readings (temperatures, humidity, fan duty, signal) only
   go out every mqtt_interval_s. */
typedef struct {
    uint16_t alarms, boost_delay_s, overrun_s, night_start_min, night_end_min;
    uint8_t online, fan_speed, status, bypass_auto_state, holiday, frost, night_enabled, humidity_setpoint, filter_pct;
    int8_t bypass_override;
} discrete_t;

static discrete_t discrete(const mvhr_state_t *st)
{
    discrete_t d;
    memset(&d, 0, sizeof(d));   /* padding too, the struct is compared with memcmp */
    d.alarms = st->alarms;
    d.boost_delay_s = st->boost_delay_s;
    d.overrun_s = st->overrun_s;
    d.night_start_min = st->night_start_min;
    d.night_end_min = st->night_end_min;
    d.online = st->online;
    d.fan_speed = st->fan_speed;
    d.status = st->status;
    d.bypass_auto_state = st->bypass_auto_state;
    d.holiday = st->holiday;
    d.frost = st->frost;
    d.night_enabled = st->night_enabled;
    d.humidity_setpoint = st->humidity_setpoint;
    d.filter_pct = st->filter_pct;
    d.bypass_override = st->bypass_override;
    return d;
}

static void ha_task(void *arg)
{
    const TickType_t interval = pdMS_TO_TICKS(settings_get()->mqtt_interval_s * 1000U);
    TickType_t last_publish = xTaskGetTickCount();
    discrete_t last = {0};
    for (;;) {
        TickType_t since = xTaskGetTickCount() - last_publish;
        if (ulTaskNotifyTake(pdTRUE, since < interval ? interval - since : 0)) {
            vTaskDelay(pdMS_TO_TICKS(COALESCE_MS));
            ulTaskNotifyTake(pdTRUE, 0);
        }
        if (s_state != HA_CONNECTED) {
            last_publish = xTaskGetTickCount();     /* the connect event kicks us */
            continue;
        }
        bool due = xTaskGetTickCount() - last_publish >= interval;
        if (s_need_discovery) {
            s_need_discovery = false;
            publish_discovery();
            esp_mqtt_client_publish(s_client, s_avail, "online", 0, 1, 1);
            due = true;
        }
        mvhr_state_t st;
        mvhr_get_state(&st);
        discrete_t now = discrete(&st);
        if (due || memcmp(&now, &last, sizeof(now)) != 0) {
            publish_state();
            last = now;
            last_publish = xTaskGetTickCount();
        }
    }
}

esp_err_t ha_start(void)
{
    const settings_t *s = settings_get();
    if (s->mode != SETTINGS_MODE_WIFI || !s->mqtt_host[0]) {
        s_state = HA_OFF;
        ESP_LOGI(TAG, "Home Assistant off (no broker configured)");
        return ESP_OK;
    }
    const char *id = settings_device_id();
    snprintf(s_base, sizeof(s_base), "vectaire/%s", id);
    snprintf(s_avail, sizeof(s_avail), "%s/status", s_base);
    snprintf(s_unit_avail, sizeof(s_unit_avail), "%s/unit", s_base);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", s_base);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = s->mqtt_host,
        .broker.address.port = s->mqtt_port ? s->mqtt_port : 1883,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = id,
        .credentials.username = s->mqtt_user[0] ? s->mqtt_user : NULL,
        .credentials.authentication.password = s->mqtt_pass[0] ? s->mqtt_pass : NULL,
        .session.last_will = {.topic = s_avail, .msg = "offline", .qos = 1, .retain = 1},
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
        .outbox.limit = 32 * 1024,     /* never let unacknowledged messages eat the internal RAM */
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    s_state = HA_CONNECTING;
    if (xTaskCreate(ha_task, "ha", 6144, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    mvhr_add_listener(on_unit_change, NULL);
    ESP_LOGI(TAG, "connecting to %s:%u as %s", s->mqtt_host, cfg.broker.address.port, id);
    return esp_mqtt_client_start(s_client);
}
