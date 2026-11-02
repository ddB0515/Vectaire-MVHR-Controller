/* Vectaire MVHR controller: startup and the user button. Built with a screen (board + ui) or headless. */
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha.h"
#include "mvhr.h"
#include "net.h"
#include "settings.h"
#include "status_led.h"
#include "web.h"
#ifndef VECTAIRE_HEADLESS
#include "board.h"
#include "ui.h"
#endif

static const char *TAG = "main";

#define RESET_HOLD_MS       5000
#define SETUP_PRESS_MS      1000    /* headless: shorter presses are ignored, so a bump does nothing */
#define SETUP_HOTSPOT_S     (15 * 60)

/* Holding the button for 5 s forgets Wi-Fi and restarts into setup. Headless, a 1-4 s press opens the setup
   hotspot without forgetting anything (and shows the device password there). */
static void button_task(void *arg)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_VECTAIRE_BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(CONFIG_VECTAIRE_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "button held: resetting network settings");
                settings_reset_network();
                esp_restart();
            }
            continue;
        }
#ifdef VECTAIRE_HEADLESS
        if (held_ms >= SETUP_PRESS_MS) {
            ESP_LOGI(TAG, "button pressed: opening the setup hotspot");
            net_start_open_provisioning(SETUP_HOTSPOT_S);
        }
#endif
        held_ms = 0;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());
#ifndef VECTAIRE_HEADLESS
    ESP_ERROR_CHECK(board_init());
#endif
#ifdef CONFIG_VECTAIRE_FORCE_UNIT_SIMULATOR
    const bool simulated = true;
#else
    const bool simulated = settings_get()->unit_simulator;
#endif
    ESP_ERROR_CHECK(mvhr_start(simulated));
    ESP_ERROR_CHECK(net_start());
    if (ha_start() != ESP_OK) {
        ESP_LOGE(TAG, "Home Assistant (MQTT) failed to start");
    }
    web_start();

#ifdef VECTAIRE_HEADLESS
    /* nothing to choose on: without Wi-Fi settings go straight to the (open, time-limited) setup hotspot */
    if (settings_get()->mode != SETTINGS_MODE_WIFI) {
        net_start_open_provisioning(SETUP_HOTSPOT_S);
    }
#else
    ui_start();
    board_backlight_set(settings_get()->backlight_pct);
#endif
    status_led_start();

    xTaskCreate(button_task, "button", 2560, NULL, 2, NULL);
}
