# Pic-Cattcha

ESP32-S3 board + OV7670 (no FIFO) fake disposable camera, ESP-IDF. No SD card:
photos are stored in a FAT partition in on-board flash and pulled to your phone over Bluetooth LE.

## Wiring (ESP32-S3)
XCLK=15 SIOD=4 SIOC=5 VSYNC=6 HREF=7 PCLK=13

D0..D7 = 11,9,8,10,12,18,17,16

PWDN unused

OV7670 RESET→3V3

Shutter button: GPIO1→GND

Flash LED: GPIO2

SIOD/SIOC need 2.2k–4.7k pull-ups to 3V3.

PSRAM is set to octal (N8R8/N16R8); for R2 modules use `CONFIG_SPIRAM_MODE_QUAD=y` in `sdkconfig.defaults`.

## Build
```
export PATH=/opt/homebrew/bin:$PATH   # arm64 cmake first
. ~/esp/esp-idf/export.sh && export PATH=/opt/homebrew/bin:$PATH
idf.py build flash monitor
```

## Using it
Shutter → pink-tinted 640x480 JPEG saved to flash (27-shot roll; hold shutter 3 s at power-on for a new roll).
The camera advertises over BLE as **Pic-Cattcha** at all times.
Get photos with `docs/index.html` (Web Bluetooth: Chrome on Android/desktop, host it over https or
`localhost`; iOS needs the Bluefy browser) or any BLE app such as nRF Connect using the protocol in `main/ble.c`.
Tick "delete from camera after download" to free flash (~4 MB, about 50 photos).

## Status light (onboard RGB)
Pink = ready · blue = phone connected · yellow x3 = roll finished · red = error (1 blink camera, 2 blinks storage, 4 blinks failed shot).

## Flash LED (GPIO2)
White LED fired during capture. Don't drive a bright LED straight from the pin: GPIO2 → 1 kΩ → base of a 2N2222 (emitter to GND);
collector → LED cathode; LED anode → 5 V through a resistor (~100 Ω for a 5 mm white LED, about 20 mA). A small indicator LED can use GPIO2 → 330 Ω → LED → GND instead.

## Tests
- Host (no hardware): `make -C test`, checks the pink filter.
- On device: aim the lens at a lit scene and run
  `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.selftest" build flash monitor`.
  Checks sensor ID, frame size/format, stuck data lines, contrast, frame rate, filter + JPEG, flash storage.
  Prints PASS/FAIL per check; RGB LED goes green (all pass) or red. Rebuild without `sdkconfig.selftest` (and delete `sdkconfig`) for normal use.

## Focus mode (adjusting the lens)
Hold the shutter while powering on; the light turns cyan after 1 s; **release before 3 s** (holding to 3 s loads a new roll).
The camera starts Wi-Fi **Pic-Cattcha** (password `pinkcamera`). Join it and open http://192.168.4.1/ for live frames
plus a sharpness score. Twist the lens until the score peaks. Power-cycle to return to normal.
