#include "autoclock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "dst.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mvhr.h"
#include "net.h"
#include "settings.h"
#include "timesync.h"

static const char *TAG = "autoclock";

#define CHECK_MS        20000
#define HOLD_MS         60000       /* after a change: let the clock read-back catch up */
#define NTP_FIRST_MS    60000       /* first network time check this long after start-up */
#define NTP_EVERY_MS    (24LL * 3600 * 1000)
#define NTP_RETRY_MS    (10LL * 60 * 1000)
#define NTP_WAIT_MS     20000
#define NTP_SERVER      "pool.ntp.org"
#define EPOCH_VALID     1700000000  /* anything before November 2023 means the time was never set */

static TaskHandle_t s_task;
static volatile bool s_ntp_force;
static int64_t s_ntp_next_ms = NTP_FIRST_MS;
static volatile autoclock_ntp_result_t s_ntp_result = AUTOCLOCK_NTP_NEVER;
static volatile int s_ntp_diff_min;
static volatile int64_t s_ntp_when_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool clock_usable(const mvhr_state_t *st)
{
    return st->online && (st->valid & MVHR_G_CLOCK) && st->clock.month >= 1 && st->clock.month <= 12 &&
           st->clock.day >= 1 && st->clock.day <= 31;
}

static esp_err_t write_clock(const mvhr_clock_t *c)
{
    return mvhr_set_clock(c->hour, c->minute, c->day, c->month, (uint16_t)(2000 + c->year));
}

/* ---------------------------------------------------------------- network time */

esp_err_t autoclock_ntp_configure(int on, int16_t tz_std_x10, uint8_t dst_rule)
{
    if (tz_std_x10 < AUTOCLOCK_TZ_MIN_X10 || tz_std_x10 > AUTOCLOCK_TZ_MAX_X10 || tz_std_x10 % 5 ||
        dst_rule > DST_RULE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_t s = *settings_get();
    s.ntp_on = on != 0;
    s.tz_std_x10 = tz_std_x10;
    s.dst_rule = dst_rule;
    ESP_LOGI(TAG, "network time %s, standard time UTC%+d.%d, summer time rule: %s", on ? "on" : "off",
             tz_std_x10 / 10, abs(tz_std_x10) % 10, dst_rule_name(dst_rule));
    esp_err_t err = settings_save(&s);
    if (err == ESP_OK && on) {
        autoclock_ntp_check_now();
    }
    return err;
}

void autoclock_ntp_check_now(void)
{
    s_ntp_force = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void ntp_finish(autoclock_ntp_result_t result, int diff_min, int64_t retry_ms)
{
    s_ntp_diff_min = diff_min;
    s_ntp_when_ms = now_ms();
    s_ntp_result = result;
    s_ntp_next_ms = now_ms() + retry_ms;
}

static void check_ntp(void)
{
    const settings_t *cfg = settings_get();
    bool force = s_ntp_force;
    s_ntp_force = false;
    if (!cfg->ntp_on || (!force && now_ms() < s_ntp_next_ms)) {
        return;
    }
    net_status_t n;
    net_get_status(&n);
    if (n.state != NET_CONNECTED) {
        ESP_LOGI(TAG, "network time: no Wi-Fi");
        ntp_finish(AUTOCLOCK_NTP_NO_WIFI, 0, NTP_RETRY_MS);
        return;
    }
    mvhr_state_t st;
    mvhr_get_state(&st);
    if (!clock_usable(&st)) {
        ESP_LOGI(TAG, "network time: the unit's clock cannot be read");
        ntp_finish(AUTOCLOCK_NTP_UNIT_OFFLINE, 0, NTP_RETRY_MS / 2);
        return;
    }

    s_ntp_result = AUTOCLOCK_NTP_CHECKING;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&sntp);
    if (err == ESP_OK) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_WAIT_MS));
    }
    esp_netif_sntp_deinit();
    time_t utc = time(NULL);
    if (err != ESP_OK || utc < EPOCH_VALID) {
        ESP_LOGW(TAG, "network time: no answer from %s (%s)", NTP_SERVER, esp_err_to_name(err));
        ntp_finish(AUTOCLOCK_NTP_NO_SYNC, 0, NTP_RETRY_MS);
        return;
    }

    mvhr_get_state(&st);        /* the clock may have moved on while we waited */
    if (!clock_usable(&st)) {
        ntp_finish(AUTOCLOCK_NTP_UNIT_OFFLINE, 0, NTP_RETRY_MS / 2);
        return;
    }
    timesync_plan_t p;
    timesync_plan((int64_t)utc, cfg->tz_std_x10 * 6, cfg->dst_rule, &st.clock, &p);
    ESP_LOGI(TAG, "network time: unit clock %02u:%02u %02u/%02u, should be %02u:%02u %02u/%02u (%s), %+d min", st.clock.hour,
             st.clock.minute, st.clock.day, st.clock.month, p.target.hour, p.target.minute, p.target.day,
             p.target.month, p.summer ? "summer time" : "standard time", -p.diff_min);

    if (p.write) {
        if (write_clock(&p.target) != ESP_OK) {
            ESP_LOGW(TAG, "network time: could not queue the clock change");
            ntp_finish(AUTOCLOCK_NTP_UNIT_OFFLINE, p.diff_min, NTP_RETRY_MS / 2);
            return;
        }
        ntp_finish(AUTOCLOCK_NTP_CORRECTED, p.diff_min, NTP_EVERY_MS);
    } else {
        ntp_finish(AUTOCLOCK_NTP_OK, p.diff_min, NTP_EVERY_MS);
    }
    if (p.write) {
        vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
    }
}

void autoclock_ntp_describe(char *out, size_t size)
{
    if (!settings_get()->ntp_on) {
        snprintf(out, size, "Network time is off.");
        return;
    }
    int64_t ago = (now_ms() - s_ntp_when_ms) / 1000;
    char when[24];
    if (ago < 90) {
        snprintf(when, sizeof(when), "%d s ago", (int)ago);
    } else if (ago < 5400) {
        snprintf(when, sizeof(when), "%d min ago", (int)(ago / 60));
    } else {
        snprintf(when, sizeof(when), "%d h ago", (int)(ago / 3600));
    }
    int d = s_ntp_diff_min;
    switch (s_ntp_result) {
    case AUTOCLOCK_NTP_NEVER:
        snprintf(out, size, "On. The first check runs shortly after start-up.");
        break;
    case AUTOCLOCK_NTP_CHECKING:
        snprintf(out, size, "Checking the time now...");
        break;
    case AUTOCLOCK_NTP_OK:
        snprintf(out, size, "Last check %s: the clock was right (%+d min).", when, -d);
        break;
    case AUTOCLOCK_NTP_CORRECTED:
        snprintf(out, size, "Last check %s: the clock was %d min %s and was corrected.", when, abs(d),
                 d > 0 ? "fast" : "slow");
        break;
    case AUTOCLOCK_NTP_NO_WIFI:
        snprintf(out, size, "Last check %s: no Wi-Fi. It tries again soon.", when);
        break;
    case AUTOCLOCK_NTP_NO_SYNC:
        snprintf(out, size, "Last check %s: no answer from the time server. It tries again soon.", when);
        break;
    case AUTOCLOCK_NTP_UNIT_OFFLINE:
    default:
        snprintf(out, size, "Last check %s: the unit's clock could not be read. It tries again soon.", when);
        break;
    }
}

/* ---------------------------------------------------------------- task */

static void autoclock_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_MS));      /* a notification means "check now" */
        check_ntp();
    }
}

void autoclock_start(void)
{
    xTaskCreate(autoclock_task, "autoclock", 4096, NULL, 3, &s_task);
}
