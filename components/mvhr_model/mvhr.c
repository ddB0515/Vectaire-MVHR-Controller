#include "mvhr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mvhr_decode.h"
#include "mvhr_sim.h"
#include "vbus.h"

static const char *TAG = "mvhr";

#define MAX_LISTENERS       4
#define WRITE_REPEATS       3       /* the original panel sends every change three times */
#define OFFLINE_AFTER       3       /* consecutive failed status polls */
#define CMD_QUEUE_LEN       16

/* Poll periods (ms) */
#define PERIOD_STATUS       500
#define PERIOD_STATUS_OFF   3000    /* while the unit doesn't answer */
#define PERIOD_CLOCK        10000
#define PERIOD_COUNTERS     60000
#define PERIOD_SETTINGS     300000
#define PERIOD_VERSION      600000
#define PERIOD_FIRE         2000
#define PERIOD_OFFLINE_LOG  30000   /* bus statistics while the unit doesn't answer */

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[12];
    uint32_t refresh;       /* groups to re-read afterwards */
} mvhr_cmd_t;

static mvhr_state_t s_state;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmds;
static TaskHandle_t s_task;
static bool s_simulated;
static struct {
    mvhr_listener_t cb;
    void *arg;
} s_listeners[MAX_LISTENERS];

/* next due time per poll group, in ms since boot; mvhr_refresh() zeroes them from other tasks */
static volatile int64_t s_due_status, s_due_clock, s_due_counters, s_due_settings, s_due_version, s_due_fire;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify(uint32_t changed)
{
    if (!changed) {
        return;
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(changed, s_listeners[i].arg);
        }
    }
}

esp_err_t mvhr_add_listener(mvhr_listener_t cb, void *arg)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].arg = arg;
            s_listeners[i].cb = cb;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void mvhr_get_state(mvhr_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

/* Run one transaction and fold the reply into the state. Returns the changed groups. */
static uint32_t exchange(uint8_t cmd, const uint8_t *data, uint8_t len, bool *ok)
{
    vbus_frame_t reply;
    *ok = vbus_transact(cmd, data, len, &reply) == ESP_OK;
    if (!*ok) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mvhr_state_t before = s_state;
    uint32_t group = mvhr_decode_reply(&s_state, cmd, reply.data, reply.len);
    s_state.valid |= group;
    bool changed = memcmp(&before, &s_state, sizeof(s_state)) != 0;
    xSemaphoreGive(s_lock);
    return changed ? group : 0;
}

static void set_online(bool online, uint32_t *changed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_state.online;
    s_state.online = online;
    xSemaphoreGive(s_lock);
    if (was != online) {
        ESP_LOGW(TAG, "unit %s", online ? "ONLINE" : "OFFLINE");
        *changed |= MVHR_G_ONLINE;
        if (online) {   /* re-read everything after (re)connecting */
            s_due_version = s_due_clock = s_due_counters = s_due_settings = 0;
        }
    }
}

static void log_status(void)
{
    mvhr_state_t s;
    mvhr_get_state(&s);
    ESP_LOGI(TAG, "status: extract %d.%d C, supply %d.%d C, RH %u.%u%%, fan %u (%u%%/%u%%), status %02x, "
                  "alarms %04x, bypass %u/%d%s, holiday %d, frost %d",
             s.temp_extract_x10 / 10, abs(s.temp_extract_x10 % 10), s.temp_supply_x10 / 10,
             abs(s.temp_supply_x10 % 10), s.humidity_x10 / 10, s.humidity_x10 % 10, s.fan_speed, s.fan_a_pct,
             s.fan_b_pct, s.status, s.alarms, s.bypass_auto_state, s.bypass_override,
             s.bypass_locked ? " locked" : "", s.holiday, s.frost);
}

static void run_command(const mvhr_cmd_t *c, uint32_t *changed)
{
    int acked = 0;
    for (int i = 0; i < WRITE_REPEATS; i++) {
        bool ok;
        *changed |= exchange(c->cmd, c->data, c->len, &ok);
        acked += ok;
    }
    ESP_LOGI(TAG, "write cmd %02x: %d/%d acknowledged", c->cmd, acked, WRITE_REPEATS);
    if (c->refresh) {
        mvhr_refresh(c->refresh);
    }
}

static void poll_group(uint8_t cmd, const uint8_t *data, uint8_t len, volatile int64_t *due, int period,
                       uint32_t *changed)
{
    bool ok;
    *changed |= exchange(cmd, data, len, &ok);
    *due = now_ms() + period;
}

/* echo frames > 0 means our TX reaches the bus and RX hears it; rx bytes without echo points at the levels */
static void log_bus_stats(void)
{
    vbus_stats_t b;
    vbus_get_stats(&b);
    ESP_LOGW(TAG, "unit not answering: requests %lu, replies %lu, timeouts %lu, rx bytes %lu, echo frames %lu, "
                  "crc errors %lu, framing errors %lu",
             (unsigned long)b.requests, (unsigned long)b.replies, (unsigned long)b.timeouts,
             (unsigned long)b.rx_bytes, (unsigned long)b.echo_frames, (unsigned long)b.crc_errors,
             (unsigned long)b.framing_errors);
}

static void model_task(void *arg)
{
    int fails = 0;
    int64_t offline_log_due = 0;
    for (;;) {
        uint32_t changed = 0;
        mvhr_cmd_t c;

        /* writes first; they are only sent while the unit answers */
        if (s_state.online && xQueueReceive(s_cmds, &c, 0) == pdTRUE) {
            run_command(&c, &changed);
            notify(changed);
            continue;
        }

        int64_t t = now_ms();
        if (t >= s_due_status) {
            bool ok;
            uint32_t g = exchange(MVHR_CMD_STATUS, NULL, 0, &ok);
            changed |= g;
            fails = ok ? 0 : fails + 1;
            set_online(ok || (s_state.online && fails < OFFLINE_AFTER), &changed);
            s_due_status = now_ms() + (s_state.online ? PERIOD_STATUS : PERIOD_STATUS_OFF);
            if (!s_state.online && now_ms() >= offline_log_due) {
                log_bus_stats();
                offline_log_due = now_ms() + PERIOD_OFFLINE_LOG;
            }
            if (g) {
                log_status();
            }
        } else if (s_state.online && t >= s_due_version) {
            poll_group(MVHR_CMD_CONFIG_READ, NULL, 0, &s_due_version, PERIOD_VERSION, &changed);
        } else if (s_state.online && t >= s_due_clock) {
            poll_group(MVHR_CMD_CLOCK_READ, NULL, 0, &s_due_clock, PERIOD_CLOCK, &changed);
        } else if (s_state.online && t >= s_due_counters) {
            poll_group(MVHR_CMD_COUNTERS, NULL, 0, &s_due_counters, PERIOD_COUNTERS, &changed);
        } else if (s_state.online && t >= s_due_settings) {
            bool ok;
            changed |= exchange(MVHR_CMD_BOOST_READ, NULL, 0, &ok);
            changed |= exchange(MVHR_CMD_OVERRUN_READ, NULL, 0, &ok);
            changed |= exchange(MVHR_CMD_HUMID_READ, NULL, 0, &ok);
            s_due_settings = now_ms() + PERIOD_SETTINGS;
        } else if (s_state.online && (s_state.alarms & MVHR_ALARM_FIRE) && t >= s_due_fire) {
            const uint8_t poll_only = 0;
            poll_group(MVHR_CMD_FIRE, &poll_only, 1, &s_due_fire, PERIOD_FIRE, &changed);
        } else {
            /* idle until the next poll is due or a command arrives */
            vbus_listen(20);
            ulTaskNotifyTake(pdTRUE, 0);
        }
        notify(changed);
    }
}

static int sim_responder(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *reply, void *arg)
{
    return mvhr_sim_reply(cmd, data, len, reply, (uint32_t)now_ms());
}

bool mvhr_is_simulated(void)
{
    return s_simulated;
}

esp_err_t mvhr_start(bool simulated)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(mvhr_cmd_t));
    ESP_RETURN_ON_FALSE(s_lock && s_cmds, ESP_ERR_NO_MEM, TAG, "alloc");
    s_state.bypass_override = MVHR_BYPASS_AUTO;

    vbus_config_t cfg = VBUS_CONFIG_DEFAULT();
    cfg.tx_gpio = CONFIG_VECTAIRE_BUS_TX_GPIO;
    cfg.rx_gpio = CONFIG_VECTAIRE_BUS_RX_GPIO;
    s_simulated = simulated;
    if (simulated) {
        mvhr_sim_init();
        ESP_RETURN_ON_ERROR(vbus_init_simulated(&cfg, sim_responder, NULL), TAG, "bus");
    } else {
        ESP_RETURN_ON_ERROR(vbus_init(&cfg), TAG, "bus");
    }
    BaseType_t ok = xTaskCreate(model_task, "mvhr", 4096, NULL, 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void mvhr_refresh(uint32_t groups)
{
    if (groups & MVHR_G_STATUS) {
        s_due_status = 0;
    }
    if (groups & MVHR_G_CLOCK) {
        s_due_clock = 0;
    }
    if (groups & MVHR_G_COUNTERS) {
        s_due_counters = 0;
    }
    if (groups & MVHR_G_SETTINGS) {
        s_due_settings = 0;
    }
    if (groups & MVHR_G_VERSION) {
        s_due_version = 0;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static esp_err_t queue_cmd(uint8_t cmd, const uint8_t *data, uint8_t len, uint32_t refresh)
{
    mvhr_cmd_t c = {.cmd = cmd, .len = len, .refresh = refresh};
    if (len > sizeof(c.data)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len) {
        memcpy(c.data, data, len);
    }
    if (xQueueSend(s_cmds, &c, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

esp_err_t mvhr_set_fan_speed(uint8_t speed)
{
    ESP_RETURN_ON_FALSE(speed >= 1 && speed <= 3, ESP_ERR_INVALID_ARG, TAG, "speed 1..3");
    const uint8_t d = speed - 1;
    return queue_cmd(MVHR_CMD_FAN, &d, 1, MVHR_G_STATUS);
}

esp_err_t mvhr_set_bypass(int8_t mode)
{
    ESP_RETURN_ON_FALSE(mode == MVHR_BYPASS_AUTO || (mode >= 0 && mode <= 2), ESP_ERR_INVALID_ARG, TAG, "mode");
    const uint8_t d = mode == MVHR_BYPASS_AUTO ? 0xFF : (uint8_t)mode;
    return queue_cmd(MVHR_CMD_BYPASS, &d, 1, MVHR_G_STATUS);
}

esp_err_t mvhr_set_holiday(bool on)
{
    const uint8_t d = on;
    return queue_cmd(MVHR_CMD_HOLIDAY, &d, 1, MVHR_G_STATUS | MVHR_G_VERSION);
}

static esp_err_t write_u16(uint8_t cmd, uint16_t v)
{
    const uint8_t d[2] = {v >> 8, v & 0xFF};
    return queue_cmd(cmd, d, 2, MVHR_G_SETTINGS);
}

esp_err_t mvhr_set_boost_delay(uint16_t seconds)
{
    ESP_RETURN_ON_FALSE(seconds <= 300, ESP_ERR_INVALID_ARG, TAG, "0..300 s");
    return write_u16(MVHR_CMD_BOOST_WRITE, seconds);
}

esp_err_t mvhr_set_overrun(uint16_t seconds)
{
    ESP_RETURN_ON_FALSE(seconds <= 90 * 60, ESP_ERR_INVALID_ARG, TAG, "0..5400 s");
    return write_u16(MVHR_CMD_OVERRUN_WRITE, seconds);
}

esp_err_t mvhr_set_humidity_setpoint(uint8_t pct)
{
    ESP_RETURN_ON_FALSE(pct >= 35 && pct <= 99, ESP_ERR_INVALID_ARG, TAG, "35..99 %");
    return write_u16(MVHR_CMD_HUMID_WRITE, pct);
}

esp_err_t mvhr_set_night_inhibition(bool enabled, uint16_t start_min, uint16_t end_min)
{
    ESP_RETURN_ON_FALSE(start_min < 1440 && end_min < 1440, ESP_ERR_INVALID_ARG, TAG, "minute of day");
    uint16_t s = start_min | (enabled ? 0x8000 : 0);
    const uint8_t d[4] = {s >> 8, s & 0xFF, end_min >> 8, end_min & 0xFF};
    return queue_cmd(MVHR_CMD_NIGHT, d, 4, MVHR_G_VERSION | MVHR_G_STATUS);
}

static esp_err_t write_clock(uint8_t cmd, const mvhr_clock_t *c)
{
    uint8_t d[9];
    mvhr_encode_clock(c, d);
    return queue_cmd(cmd, d, 9, MVHR_G_CLOCK);
}

esp_err_t mvhr_set_time(uint8_t hour, uint8_t minute)
{
    ESP_RETURN_ON_FALSE(hour < 24 && minute < 60, ESP_ERR_INVALID_ARG, TAG, "time");
    mvhr_state_t s;
    mvhr_get_state(&s);
    ESP_RETURN_ON_FALSE(s.valid & MVHR_G_CLOCK, ESP_ERR_INVALID_STATE, TAG, "clock not read yet");
    s.clock.hour = hour;
    s.clock.minute = minute;
    return write_clock(MVHR_CMD_CLOCK_TIME, &s.clock);
}

esp_err_t mvhr_set_date(uint8_t day, uint8_t month, uint16_t year)
{
    ESP_RETURN_ON_FALSE(day >= 1 && day <= 31 && month >= 1 && month <= 12 && year >= 2000 && year < 2100,
                        ESP_ERR_INVALID_ARG, TAG, "date");
    mvhr_state_t s;
    mvhr_get_state(&s);
    ESP_RETURN_ON_FALSE(s.valid & MVHR_G_CLOCK, ESP_ERR_INVALID_STATE, TAG, "clock not read yet");
    s.clock.day = day;
    s.clock.month = month;
    s.clock.year = (uint8_t)(year - 2000);
    return write_clock(MVHR_CMD_CLOCK_DATE, &s.clock);
}

/* Time and date as two writes of the same block: separate mvhr_set_time()/mvhr_set_date() calls would each
   carry the other half from the old clock and undo each other. */
esp_err_t mvhr_set_clock(uint8_t hour, uint8_t minute, uint8_t day, uint8_t month, uint16_t year)
{
    ESP_RETURN_ON_FALSE(hour < 24 && minute < 60 && day >= 1 && day <= 31 && month >= 1 && month <= 12 &&
                        year >= 2000 && year < 2100, ESP_ERR_INVALID_ARG, TAG, "clock");
    mvhr_state_t s;
    mvhr_get_state(&s);
    ESP_RETURN_ON_FALSE(s.valid & MVHR_G_CLOCK, ESP_ERR_INVALID_STATE, TAG, "clock not read yet");
    s.clock.hour = hour;
    s.clock.minute = minute;
    s.clock.day = day;
    s.clock.month = month;
    s.clock.year = (uint8_t)(year - 2000);
    ESP_RETURN_ON_ERROR(write_clock(MVHR_CMD_CLOCK_TIME, &s.clock), TAG, "time");
    return write_clock(MVHR_CMD_CLOCK_DATE, &s.clock);
}

esp_err_t mvhr_reset_filter(void)
{
    return queue_cmd(MVHR_CMD_FILTER_RESET, NULL, 0, MVHR_G_COUNTERS);
}

esp_err_t mvhr_fire_alarm_resume(void)
{
    const uint8_t d = 1;
    return queue_cmd(MVHR_CMD_FIRE, &d, 1, MVHR_G_STATUS);
}
