#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "focus.h"

static const char *TAG = "focus";

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Pic-Cattcha focus</title><style>"
"body{font-family:system-ui,sans-serif;background:#2a1520;color:#ffd6e8;margin:0;padding:12px;text-align:center}"
"img{width:100%;max-width:640px;border-radius:8px;background:#000}"
".n{font-size:56px;font-weight:700}.b{height:18px;background:#3b1d2c;border-radius:9px;overflow:hidden;margin:8px auto;max-width:640px}"
".b i{display:block;height:100%;background:#ff7ab8;width:0}button{background:#ff7ab8;color:#2a1520;border:0;border-radius:8px;padding:10px 14px;font-size:16px}"
"</style></head><body><h3>Twist the lens until the score peaks</h3>"
"<img id=v><div class=n id=s>-</div><div class=b><i id=bar></i></div>"
"<div>peak <b id=p>0</b> <button onclick='pk=0'>reset peak</button></div>"
"<script>let pk=0,u;async function loop(){try{const r=await fetch('/snap?'+Date.now());"
"const sc=+r.headers.get('X-Focus');const b=await r.blob();const n=URL.createObjectURL(b);"
"v.src=n;if(u)URL.revokeObjectURL(u);u=n;s.textContent=sc;if(sc>pk)pk=sc;p.textContent=pk;"
"bar.style.width=Math.min(100,pk?sc/pk*100:0)+'%'}catch(e){await new Promise(r=>setTimeout(r,500))}setTimeout(loop,0)}loop()</script>"
"</body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snap_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
    uint32_t score = focus_score(fb->buf, fb->width, fb->height);
    uint8_t *jpg = NULL;
    size_t len = 0;
    bool ok = frame2jpg(fb, 60, &jpg, &len);
    esp_camera_fb_return(fb);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }
    char sc[16];
    snprintf(sc, sizeof(sc), "%lu", (unsigned long)score);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "X-Focus", sc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return r;
}

void focus_mode_run(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t ap = { .ap = {
        .ssid = "Pic-Cattcha", .ssid_len = 11, .channel = 1, .password = "pinkcamera",
        .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = page_handler };
    httpd_uri_t snap = { .uri = "/snap", .method = HTTP_GET, .handler = snap_handler };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &snap);
    ESP_LOGI(TAG, "Focus mode: join Wi-Fi 'Pic-Cattcha' (password pinkcamera), open http://192.168.4.1/");
}
