#pragma once
/* Status LED for controllers without a screen (CONFIG_VECTAIRE_LED_*): setup, Wi-Fi, unit and alarm state. */

#ifdef __cplusplus
extern "C" {
#endif

/* Start the LED task; does nothing when no LED is configured. */
void status_led_start(void);

#ifdef __cplusplus
}
#endif
