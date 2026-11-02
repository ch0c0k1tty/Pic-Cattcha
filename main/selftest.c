// On-device self-test. Point the lens at a normal, lit scene (not a lens cap / blank wall)
// and run: idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.selftest" build flash monitor
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "film_filter.h"
#include "ble.h"
#include "selftest.h"

static const char *TAG = "selftest";
static int s_pass, s_fail;

static void result(const char *name, bool ok, const char *detail)
{
    if (ok) { s_pass++; printf("PASS  %s\n", name); }
    else    { s_fail++; printf("FAIL  %s  -- %s\n", name, detail); }
}

static camera_fb_t *grab_settled(void)
{
    // OV7670 auto exposure/white balance needs a few frames.
    for (int i = 0; i < 5; i++) {
        camera_fb_t *j = esp_camera_fb_get();
        if (j) esp_camera_fb_return(j);
    }
    return esp_camera_fb_get();
}

static void test_sensor_id(void)
{
    sensor_t *s = esp_camera_sensor_get();
    char d[64];
    snprintf(d, sizeof(d), "got PID 0x%02x, expected 0x%02x (check SIOD/SIOC, pull-ups, XCLK)",
             s ? s->id.PID : 0, OV7670_PID);
    result("sensor is an OV7670", s && s->id.PID == OV7670_PID, d);
}

static void test_frame_geometry(void)
{
    camera_fb_t *fb = grab_settled();
    char d[96] = "no frame (check VSYNC/HREF/PCLK/XCLK)";
    bool ok = fb != NULL;
    if (fb) {
        ok = fb->width == 640 && fb->height == 480 && fb->format == PIXFORMAT_RGB565 &&
             fb->len == 640 * 480 * 2;
        snprintf(d, sizeof(d), "got %ux%u fmt %d len %u", (unsigned)fb->width, (unsigned)fb->height,
                 (int)fb->format, (unsigned)fb->len);
        esp_camera_fb_return(fb);
    }
    result("frame is 640x480 RGB565", ok, d);
}

static void test_data_lines(void)
{
    camera_fb_t *fb = grab_settled();
    if (!fb) { result("all 8 data lines toggle", false, "no frame"); return; }
    uint8_t seen_or = 0, seen_and = 0xFF;
    for (size_t i = 0; i < fb->len; i++) { seen_or |= fb->buf[i]; seen_and &= fb->buf[i]; }
    esp_camera_fb_return(fb);
    uint8_t stuck = (uint8_t)(~seen_or | seen_and);   // bit never 1, or never 0
    char d[96];
    snprintf(d, sizeof(d), "stuck data bits mask 0x%02x (D0..D7 = bit 0..7) - wiring, or lens covered", stuck);
    result("all 8 data lines toggle", stuck == 0, d);
}

static void test_image_not_flat(void)
{
    camera_fb_t *fb = grab_settled();
    if (!fb) { result("image has contrast", false, "no frame"); return; }
    uint8_t mn = 255, mx = 0;
    for (size_t i = 0; i < fb->len; i += 2) {       // high byte of each pixel
        if (fb->buf[i] < mn) mn = fb->buf[i];
        if (fb->buf[i] > mx) mx = fb->buf[i];
    }
    esp_camera_fb_return(fb);
    char d[64];
    snprintf(d, sizeof(d), "high-byte range %u..%u, scene too flat/dark", mn, mx);
    result("image has contrast", mx - mn >= 24, d);
}

static void test_frame_rate(void)
{
    int64_t t0 = esp_timer_get_time();
    int got = 0;
    for (int i = 0; i < 10; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) { got++; esp_camera_fb_return(fb); }
    }
    float secs = (esp_timer_get_time() - t0) / 1e6f;
    char d[64];
    snprintf(d, sizeof(d), "%d/10 frames in %.2fs", got, secs);
    printf("      (%s)\n", d);
    result("10 frames grabbed at >= 2 fps", got == 10 && secs < 5.0f, d);
}

static void test_filter_and_jpeg(void)
{
    camera_fb_t *fb = grab_settled();
    if (!fb) { result("filter + JPEG encode", false, "no frame"); return; }
    uint8_t first[16];
    memcpy(first, fb->buf, sizeof(first));
    film_filter_apply(fb->buf, fb->width, fb->height, (uint32_t)esp_timer_get_time());
    bool changed = memcmp(first, fb->buf, sizeof(first)) != 0;

    uint8_t *jpg = NULL;
    size_t len = 0;
    bool ok = frame2jpg(fb, 80, &jpg, &len);
    esp_camera_fb_return(fb);
    bool valid = ok && len > 1000 && jpg[0] == 0xFF && jpg[1] == 0xD8 &&
                 jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9;
    char d[64];
    snprintf(d, sizeof(d), "encode ok=%d len=%u", ok, (unsigned)len);
    printf("      (JPEG %u bytes)\n", (unsigned)len);
    result("filter changes pixels", changed, "filter left the frame untouched");
    result("JPEG has SOI/EOI markers", valid, d);
    free(jpg);
}

static void test_flash_storage(void)
{
    const char *path = PHOTO_DIR "/SELFTEST.TMP";
    const size_t N = 4096;
    uint8_t *pattern = malloc(N), *back = malloc(N);   // heap: main task stack is small
    if (!pattern || !back) { free(pattern); free(back); result("flash storage write/read-back", false, "out of memory"); return; }
    for (size_t i = 0; i < N; i++) pattern[i] = (uint8_t)(i * 31 + 7);
    bool w = false, r = false;
    FILE *f = fopen(path, "wb");
    if (f) { w = fwrite(pattern, 1, N, f) == N; fclose(f); }
    f = fopen(path, "rb");
    if (f) { r = fread(back, 1, N, f) == N && memcmp(pattern, back, N) == 0; fclose(f); }
    remove(path);
    free(pattern); free(back);
    result("flash storage write/read-back", w && r, w ? "read-back mismatch" : "cannot write (partition 'storage' missing?)");
    FILE *g = fopen(path, "rb");
    result("test file removed", g == NULL, "file still present after remove");
    if (g) fclose(g);
}

int selftest_run(void)
{
    s_pass = s_fail = 0;
    printf("\n===== Pic-Cattcha self-test (aim lens at a lit scene) =====\n");
    vTaskDelay(pdMS_TO_TICKS(1500));       // let sensor start streaming
    test_sensor_id();
    test_frame_geometry();
    test_data_lines();
    test_image_not_flat();
    test_frame_rate();
    test_filter_and_jpeg();
    test_flash_storage();
    printf("===== SELFTEST: %d passed, %d failed =====\n", s_pass, s_fail);
    ESP_LOGI(TAG, "%s", s_fail ? "SELFTEST FAILED" : "SELFTEST OK");
    return s_fail;
}
