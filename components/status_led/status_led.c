/* One LED shows the most important state as a blink pattern (and, on a WS2812, a colour):
     setup hotspot open      blue    blinks once a second
     fire alarm              red     flickers fast
     unit not responding     red     double flash every 2 s
     Wi-Fi connecting        yellow  short flash every 2 s
     other alarm             orange  on, short gap every 2 s
     all fine                green   on (dim) */
#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mvhr.h"
#include "net.h"
#include "sdkconfig.h"
#if CONFIG_VECTAIRE_LED_WS2812
#include "led_strip.h"
#endif

#if !CONFIG_VECTAIRE_LED_NONE

static const char *TAG = "led";

#define STEP_MS     100
#define STEPS       20      /* one pattern = 2 s */

typedef struct {
    uint32_t on;            /* bit i: lit during step i */
    uint8_t r, g, b;        /* WS2812 colour, kept dim */
} pattern_t;

static const pattern_t P_SETUP = {0x07c1f, 0, 0, 40};
static const pattern_t P_FIRE = {0x55555, 60, 0, 0};
static const pattern_t P_NO_UNIT = {0x00033, 60, 0, 0};
static const pattern_t P_CONNECTING = {0x00003, 40, 30, 0};
static const pattern_t P_ALARM = {0xffffc, 50, 15, 0};
static const pattern_t P_OK = {0xfffff, 0, 20, 0};

static const pattern_t *current(void)
{
    net_status_t n;
    net_get_status(&n);
    mvhr_state_t st;
    mvhr_get_state(&st);
    bool status = st.online && (st.valid & MVHR_G_STATUS);
    if (status && (st.alarms & MVHR_ALARM_FIRE)) {
        return &P_FIRE;
    }
    if (n.state == NET_PROVISIONING || n.state == NET_TESTING) {
        return &P_SETUP;
    }
    if (!st.online) {
        return &P_NO_UNIT;
    }
    if (n.state == NET_CONNECTING) {
        return &P_CONNECTING;
    }
    if (status && st.alarms) {
        return &P_ALARM;
    }
    return &P_OK;
}

#if CONFIG_VECTAIRE_LED_WS2812
static led_strip_handle_t s_strip;

static bool led_init(void)
{
    const led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_VECTAIRE_LED_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rc = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    return led_strip_new_rmt_device(&sc, &rc, &s_strip) == ESP_OK;
}

static void led_set(const pattern_t *p, bool on)
{
    if (on) {
        led_strip_set_pixel(s_strip, 0, p->r, p->g, p->b);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}
#else
static bool led_init(void)
{
    const gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_VECTAIRE_LED_PIN, .mode = GPIO_MODE_OUTPUT};
    return gpio_config(&io) == ESP_OK;
}

static void led_set(const pattern_t *p, bool on)
{
#if CONFIG_VECTAIRE_LED_ACTIVE_LOW
    on = !on;
#endif
    gpio_set_level(CONFIG_VECTAIRE_LED_PIN, on);
}
#endif

static void led_task(void *arg)
{
    const pattern_t *last = NULL;
    bool lit = false;
    for (int step = 0;; step = (step + 1) % STEPS) {
        const pattern_t *p = current();
        bool on = p->on & (1UL << step);
        if (p != last || on != lit) {
            led_set(p, on);
            last = p;
            lit = on;
        }
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

void status_led_start(void)
{
    if (!led_init()) {
        ESP_LOGE(TAG, "status LED on GPIO%d failed", CONFIG_VECTAIRE_LED_PIN);
        return;
    }
    xTaskCreate(led_task, "led", 2560, NULL, 1, NULL);
}

#else

void status_led_start(void)
{
}

#endif
