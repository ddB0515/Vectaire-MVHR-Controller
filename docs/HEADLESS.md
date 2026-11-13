# Headless controller (no screen)

The same firmware without the screen: bus, Home Assistant (MQTT), the web pages and OTA. It runs on a classic ESP32,
ESP32-C3, -S3, -C5 or -C6 board with at least 4 MB flash. PSRAM is not needed. Everything is set up and operated from
the web pages (`http://vectaire-xxxx.local/`) and Home Assistant.

## Build and flash

```bash
tools/idf.sh --headless esp32c3 build                          # build-headless-esp32c3/
tools/idf.sh --headless esp32c3 -p /dev/ttyUSB0 flash          # first time, over USB
tools/ota.sh vectaire-xxxx.local --headless esp32c3            # later, over Wi-Fi
tools/idf.sh --headless esp32c3 menuconfig                     # pins and LED: menu "Vectaire"
```

Each chip has its own build folder and `sdkconfig`. The display build (`tools/idf.sh build`, `build/`, `sdkconfig`)
is not affected. The two variants have different project names, so OTA refuses to install a display image on a
headless controller, and the other way round.

## Default pins

| Chip | Bus TX (to NPN base) | Bus RX (from divider) | Button | Status LED |
|---|---|---|---|---|
| ESP32 | GPIO25 | GPIO26 | GPIO0 (BOOT) | GPIO2, single LED (DevKit V1) |
| ESP32-S3 | GPIO17 | GPIO18 | GPIO0 (BOOT) | GPIO48, WS2812 (DevKitC-1 v1.0; v1.1 uses GPIO38) |
| ESP32-C3 | GPIO4 | GPIO5 | GPIO9 (BOOT) | GPIO8, WS2812 (DevKitM-1) |
| ESP32-C6 | GPIO2 | GPIO3 | GPIO9 (BOOT) | GPIO8, WS2812 (DevKitC-1) |
| ESP32-C5 | GPIO2 | GPIO3 | GPIO28 (BOOT) | GPIO27, WS2812 (DevKitC-1) |

The bus circuit is the same as on the display board:
- **TX:** the GPIO drives an NPN transistor through its base resistor (100k from base to GND), and the transistor pulls
  the 12 V bus low.
- **RX:** the GPIO reads the bus through a 15k/4.7k divider.
- **GND** is shared with the unit.

For a C3 "Super Mini" (plain blue LED on GPIO8), choose the "Single LED on a GPIO" option with "active low" in
menuconfig.

## First setup

1. Power the controller up. Without saved Wi-Fi settings it opens the **open** hotspot `Vectaire-XXXX` (no
   password). The LED blinks blue, or blinks once a second on a single LED.
2. Join the hotspot with a phone. The setup page opens by itself, or go to `http://192.168.4.1/`.
3. The setup page shows the **device password**. Write it down: firmware updates and the Home Assistant settings page
   ask for it.
4. Choose your Wi-Fi network. Home Assistant (MQTT) is optional here. Then press Connect.

The hotspot closes after **15 minutes with nobody joined**. A controller that is already set up then restarts into
normal operation; one that isn't set up stays without Wi-Fi until it is powered up again or the button is pressed.
Because the hotspot is open, anyone nearby could join it while it is up. Keep the setup short.

## Button

| Press | Effect |
|---|---|
| 1–4 s | Opens the setup hotspot without forgetting anything. Use it to change the Wi-Fi or to see the device password again. |
| 5 s | Forgets the Wi-Fi settings and restarts into setup. MQTT settings are kept. |

## Status LED

| State | WS2812 | Single LED |
|---|---|---|
| Fire alarm | red, fast flicker | fast flicker |
| Setup hotspot open | blue, blinks once a second | blinks once a second |
| Unit not responding | red, double flash every 2 s | double flash every 2 s |
| Connecting to Wi-Fi | yellow, short flash every 2 s | short flash every 2 s |
| Other alarm (e.g. filters) | orange, on with a short gap | on with a short gap |
| All fine | green, on | on |
