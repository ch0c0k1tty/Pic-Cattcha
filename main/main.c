// Pic-Cattcha: a fake disposable camera built on an ESP32-CAM-style board + OV7670.
//
// Press the shutter -> a pink-tinted, grainy, vignetted JPEG is stored in on-board flash; fetch them over Bluetooth LE.
// A "film roll" of 27 shots is tracked in NVS. Hold the shutter while powering on
// for 3 s to load a fresh roll.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "ble.h"
#include "film_filter.h"
#include "selftest.h"
#include "focus.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

static const char *TAG = "cattcha";

// ---- Config ----------------------------------------------------------------
#define FILM_ROLL_SHOTS   27
#define JPEG_QUALITY      92
#define WIND_TIME_MS      1200   // fake "advance the film wheel" delay between shots

#define PIN_SHUTTER       1     // button to GND (internal pull-up)
#define PIN_FLASH         2     // external white flash LED, driven via transistor (see README)
#define PIN_RGB_LED       38    // onboard WS2812: GPIO48 (DevKitC-1 v1.0) or GPIO38 (v1.1)

// OV7670 wiring for ESP32-S3 (LCD_CAM peripheral; data pins are freely routable).
// Avoids strapping pins (0,3,45,46), native USB (19,20) and flash/PSRAM pins (26-37).
#define CAM_PIN_PWDN      -1
#define CAM_PIN_RESET     -1     // tie OV7670 RESET to 3V3
#define CAM_PIN_XCLK       15
#define CAM_PIN_SIOD      4
#define CAM_PIN_SIOC      5
#define CAM_PIN_D7        16
#define CAM_PIN_D6        17
#define CAM_PIN_D5        18
#define CAM_PIN_D4        12
#define CAM_PIN_D3        10
#define CAM_PIN_D2        8
#define CAM_PIN_D1        9
#define CAM_PIN_D0         11
#define CAM_PIN_VSYNC     6
#define CAM_PIN_HREF      7
#define CAM_PIN_PCLK      13

// ---- Film counter (NVS) ----------------------------------------------------
static nvs_handle_t s_nvs;
static uint8_t  s_shots_left;
static uint32_t s_next_index;

static void film_load(void)
{
    ESP_ERROR_CHECK(nvs_open("film", NVS_READWRITE, &s_nvs));
    if (nvs_get_u8(s_nvs, "left", &s_shots_left) != ESP_OK) s_shots_left = FILM_ROLL_SHOTS;
    if (nvs_get_u32(s_nvs, "next", &s_next_index) != ESP_OK) s_next_index = 1;
}

static void film_save(void)
{
    nvs_set_u8(s_nvs, "left", s_shots_left);
    nvs_set_u32(s_nvs, "next", s_next_index);
    nvs_commit(s_nvs);
}

static void film_new_roll(void);
static uint8_t film_new_roll_from_phone(void) { film_new_roll(); return s_shots_left; }

static void film_new_roll(void)
{
    s_shots_left = FILM_ROLL_SHOTS;
    film_save();
    ESP_LOGI(TAG, "New roll loaded: %d shots", FILM_ROLL_SHOTS);
}

// ---- Hardware setup ---------------------------------------------------------
static esp_err_t camera_start(framesize_t size)
{
    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN, .pin_reset = CAM_PIN_RESET, .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD, .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6, .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2, .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC, .pin_href = CAM_PIN_HREF, .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,   // OV7670 has no JPEG engine; we encode in software
        .frame_size = size,                 // VGA 640x480 for photos
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg);
}

static esp_err_t storage_start(void)
{
    // Photos live in a FAT partition ("storage") in on-board flash, no SD card needed.
    static wl_handle_t wl;
    esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = true, .max_files = 4, .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(PHOTO_DIR, "storage", &mnt, &wl);
}

// ---- Status RGB LED ----------------------------------------------------------------
static led_strip_handle_t s_led;

typedef struct { uint8_t r, g, b; } rgb_t;
static const rgb_t C_OFF = {0, 0, 0}, C_READY = {50, 10, 30}, C_BLE = {0, 10, 50},
                   C_ERR = {80, 0, 0}, C_WARN = {60, 40, 0}, C_FOCUS = {0, 40, 40};
#if CONFIG_PIC_CATTCHA_SELFTEST
static const rgb_t C_OK = {0, 40, 0};
#endif

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}
static void led_color(rgb_t c) { led_set(c.r, c.g, c.b); }

static void gpio_start(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_SHUTTER, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    gpio_config_t fl = { .pin_bit_mask = 1ULL << PIN_FLASH, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&fl);
    gpio_set_level(PIN_FLASH, 0);

    led_strip_config_t strip = { .strip_gpio_num = PIN_RGB_LED, .max_leds = 1, .led_model = LED_MODEL_WS2812 };
    led_strip_rmt_config_t rmt = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&strip, &rmt, &s_led) != ESP_OK) { s_led = NULL; ESP_LOGE(TAG, "RGB LED init failed on GPIO%d", PIN_RGB_LED); }
    led_set(0, 0, 0);
}

static inline bool shutter_down(void) { return gpio_get_level(PIN_SHUTTER) == 0; }

static void blink(int times, int ms, rgb_t c)
{
    for (int i = 0; i < times; i++) {
        led_color(c);     vTaskDelay(pdMS_TO_TICKS(ms));
        led_color(C_OFF); vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

// ---- Take a picture ------------------------------------------------------------
static bool take_photo(void)
{
    // OV7670 auto-exposure/white balance needs a few frames after idle; discard them.
    // The flash stays on from here until the real frame is grabbed, so exposure settles under it.
    gpio_set_level(PIN_FLASH, 1);
    for (int i = 0; i < 3; i++) {
        camera_fb_t *junk = esp_camera_fb_get();
        if (junk) esp_camera_fb_return(junk);
    }
    camera_fb_t *fb = esp_camera_fb_get();
    gpio_set_level(PIN_FLASH, 0);
    if (!fb) { ESP_LOGE(TAG, "capture failed"); return false; }

    film_filter_apply(fb->buf, fb->width, fb->height, (uint32_t)esp_timer_get_time());

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg, &jpg_len);
    esp_camera_fb_return(fb);
    if (!ok) { ESP_LOGE(TAG, "jpeg encode failed"); return false; }

    char path[48];
    snprintf(path, sizeof(path), PHOTO_DIR "/IMG_%04lu.JPG", (unsigned long)s_next_index);
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "cannot open %s", path); free(jpg); return false; }
    size_t written = fwrite(jpg, 1, jpg_len, f);
    fclose(f);
    free(jpg);
    if (written != jpg_len) { ESP_LOGE(TAG, "short write (flash full?) %s", path); remove(path); return false; }

    s_next_index++;
    s_shots_left--;
    film_save();
    ble_cattcha_set_shots_left(s_shots_left);
    ESP_LOGI(TAG, "Saved %s (%u bytes), %u shots left", path, (unsigned)jpg_len, s_shots_left);
    return true;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_start();
    film_load();

    // Shutter held at power-on: release between 1 and 3 s -> focus mode (Wi-Fi live view);
    // keep holding for 3 s -> new roll.
    bool focus_mode = false;
    if (shutter_down()) {
        int held = 0;
        while (shutter_down() && held < 3000) {
            vTaskDelay(pdMS_TO_TICKS(50)); held += 50;
            if (held == 1000) led_color(C_FOCUS);       // cyan: release now for focus mode
        }
        if (held >= 3000) { film_new_roll(); blink(5, 80, C_READY); }
        else if (held >= 1000) focus_mode = true;
        else led_color(C_OFF);
        while (shutter_down()) vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (camera_start(FRAMESIZE_VGA) != ESP_OK) { ESP_LOGE(TAG, "camera init failed, check OV7670 wiring"); for (;;) { blink(1, 100, C_ERR); } }
    if (storage_start() != ESP_OK) { ESP_LOGE(TAG, "flash storage mount failed"); for (;;) { blink(2, 100, C_ERR); vTaskDelay(pdMS_TO_TICKS(600)); } }
    if (focus_mode) {
        led_color(C_FOCUS);
        // Smaller frames encode much faster; restart the driver so its buffers match.
        esp_camera_deinit();
        if (camera_start(FRAMESIZE_QVGA) != ESP_OK) { ESP_LOGE(TAG, "focus camera init failed"); for (;;) blink(1, 100, C_ERR); }
        focus_mode_run();
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#if CONFIG_PIC_CATTCHA_SELFTEST
    int failed = selftest_run();            // green = all passed, red = see serial log
    for (;;) { led_color(failed ? C_ERR : C_OK); vTaskDelay(pdMS_TO_TICKS(1000)); }
#endif
    ble_cattcha_set_shots_left(s_shots_left);
    ble_cattcha_start(film_new_roll_from_phone);

    ESP_LOGI(TAG, "Ready. %u shots left.", s_shots_left);
    blink(1, 150, C_READY);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        static int last_level = -1;
        int level = gpio_get_level(PIN_SHUTTER);
        if (level != last_level) { last_level = level; ESP_LOGI(TAG, "shutter pin GPIO%d = %d", PIN_SHUTTER, level); }
        static int idle = -1;
        int want = ble_cattcha_is_connected();
        if (want != idle) { idle = want; led_color(want ? C_BLE : C_READY); }
        if (!shutter_down()) continue;
        vTaskDelay(pdMS_TO_TICKS(30));            // debounce
        if (!shutter_down()) continue;

        if (s_shots_left == 0) {                  // roll finished
            ESP_LOGW(TAG, "Film roll finished. Reload by holding shutter at power-on.");
            blink(3, 60, C_WARN);
        } else {
            led_color(C_OFF);                     // keep the RGB light out of the shot
            bool ok = take_photo();
            led_color(idle ? C_BLE : C_READY);
            if (ok) vTaskDelay(pdMS_TO_TICKS(WIND_TIME_MS));  // winding the wheel
            else blink(4, 50, C_ERR);
        }
        while (shutter_down()) vTaskDelay(pdMS_TO_TICKS(20)); // wait for release
    }
}
