// BLE photo transfer for Pic-Cattcha (NimBLE).
//
// Service  ca77c0de-0001-4a5e-9c1d-0123456789ab
//   Control ...0002  (write)   [cmd, id_lo, id_hi]
//       1 = LIST   2 = GET id   3 = DELETE id   4 = DELETE ALL   5 = NEW ROLL
//   Data    ...0003  (notify)  first byte = packet type
//       0x01 list entries: repeated {id u16, size u32}   (little endian)
//       0x02 photo header: {id u16, size u32}
//       0x03 photo chunk : raw JPEG bytes follow
//       0x04 end of response
//       0x05 error       : {code}
//   Status  ...0004  (read/notify) {shots_left u8, photo_count u16}

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble.h"

static const char *TAG = "ble";

#define SVC_UUID(n) BLE_UUID128_INIT(0xab,0x89,0x67,0x45,0x23,0x01,0x1d,0x9c,0x5e,0x4a,(n),0x00,0xde,0xc0,0x77,0xca)
static const ble_uuid128_t svc_uuid    = SVC_UUID(0x01);
static const ble_uuid128_t ctl_uuid    = SVC_UUID(0x02);
static const ble_uuid128_t data_uuid   = SVC_UUID(0x03);
static const ble_uuid128_t status_uuid = SVC_UUID(0x04);

enum { CMD_LIST = 1, CMD_GET = 2, CMD_DELETE = 3, CMD_DELETE_ALL = 4, CMD_NEW_ROLL = 5 };
enum { PKT_LIST = 1, PKT_HEADER = 2, PKT_CHUNK = 3, PKT_END = 4, PKT_ERROR = 5 };

typedef struct { uint8_t cmd; uint16_t id; } cmd_t;

static uint16_t s_data_handle, s_status_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_data_sub;
static uint8_t s_own_addr_type;
static uint8_t s_shots_left;
static QueueHandle_t s_cmd_q;
static uint8_t (*s_on_new_roll)(void);

// ---- Photo storage helpers ---------------------------------------------------
static size_t file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n < 0 ? 0 : (size_t)n;
}

static uint16_t count_photos(void)
{
    uint16_t n = 0;
    DIR *d = opendir(PHOTO_DIR);
    if (!d) return 0;
    struct dirent *e;
    unsigned id;
    while ((e = readdir(d))) if (sscanf(e->d_name, "IMG_%u.JPG", &id) == 1) n++;
    closedir(d);
    return n;
}

// ---- Notifications ---------------------------------------------------------------
static bool notify(const uint8_t *buf, size_t len)
{
    for (int tries = 0; tries < 300; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_data_sub) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        if (!om) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        int rc = ble_gatts_notify_custom(s_conn, s_data_handle, om);
        if (rc == 0) return true;
        if (rc != BLE_HS_ENOMEM) return false;
        vTaskDelay(pdMS_TO_TICKS(10));   // out of mbufs, let the radio drain
    }
    return false;
}

static void send_simple(uint8_t type, uint8_t arg)
{
    uint8_t p[2] = { type, arg };
    notify(p, type == PKT_ERROR ? 2 : 1);
}

static void do_list(void)
{
    uint8_t pkt[1 + 6 * 30];
    size_t max_entries = (ble_att_mtu(s_conn) - 3 - 1) / 6;
    if (max_entries > 30) max_entries = 30;
    if (max_entries == 0) max_entries = 1;
    size_t n = 0;
    pkt[0] = PKT_LIST;

    DIR *d = opendir(PHOTO_DIR);
    if (d) {
        struct dirent *e;
        unsigned id;
        while ((e = readdir(d))) {
            if (sscanf(e->d_name, "IMG_%u.JPG", &id) != 1) continue;
            char path[48];
            snprintf(path, sizeof(path), PHOTO_DIR "/IMG_%04u.JPG", id);
            uint32_t sz = file_size(path);
            uint8_t *q = &pkt[1 + n * 6];
            q[0] = id & 0xFF; q[1] = id >> 8;
            q[2] = sz; q[3] = sz >> 8; q[4] = sz >> 16; q[5] = sz >> 24;
            if (++n == max_entries) { notify(pkt, 1 + n * 6); n = 0; }
        }
        closedir(d);
    }
    if (n) notify(pkt, 1 + n * 6);
    send_simple(PKT_END, 0);
}

static void do_get(uint16_t id)
{
    char path[48];
    snprintf(path, sizeof(path), PHOTO_DIR "/IMG_%04u.JPG", id);
    FILE *f = fopen(path, "rb");
    if (!f) { send_simple(PKT_ERROR, 1); return; }
    fseek(f, 0, SEEK_END);
    uint32_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t hdr[7] = { PKT_HEADER, id & 0xFF, id >> 8, sz, sz >> 8, sz >> 16, sz >> 24 };
    bool ok = notify(hdr, sizeof(hdr));

    size_t chunk = ble_att_mtu(s_conn) - 3 - 1;
    uint8_t buf[1 + 244];
    if (chunk > 244) chunk = 244;
    buf[0] = PKT_CHUNK;
    while (ok) {
        size_t n = fread(&buf[1], 1, chunk, f);
        if (n == 0) break;
        ok = notify(buf, 1 + n);
    }
    fclose(f);
    if (ok) send_simple(PKT_END, 0); else ESP_LOGW(TAG, "transfer of %s aborted", path);
}

static void do_delete(uint16_t id)
{
    char path[48];
    snprintf(path, sizeof(path), PHOTO_DIR "/IMG_%04u.JPG", id);
    if (remove(path) != 0) { send_simple(PKT_ERROR, 1); return; }
    send_simple(PKT_END, 0);
}

static void do_delete_all(void)
{
    DIR *d = opendir(PHOTO_DIR);
    if (d) {
        struct dirent *e;
        unsigned id;
        while ((e = readdir(d))) {
            if (sscanf(e->d_name, "IMG_%u.JPG", &id) != 1) continue;
            char path[48];
            snprintf(path, sizeof(path), PHOTO_DIR "/IMG_%04u.JPG", id);
            remove(path);
        }
        closedir(d);
    }
    send_simple(PKT_END, 0);
}

static void transfer_task(void *arg)
{
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.cmd) {
        case CMD_LIST:       do_list(); break;
        case CMD_GET:        do_get(c.id); break;
        case CMD_DELETE:     do_delete(c.id); break;
        case CMD_DELETE_ALL: do_delete_all(); break;
        case CMD_NEW_ROLL:
            if (s_on_new_roll) { s_shots_left = s_on_new_roll(); send_simple(PKT_END, 0); }
            else send_simple(PKT_ERROR, 3);
            break;
        default:             send_simple(PKT_ERROR, 2); break;
        }
        ble_cattcha_set_shots_left(s_shots_left);   // refresh photo count on phone
    }
}

// ---- GATT --------------------------------------------------------------------------
static int ctl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t b[3] = {0};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), &len) != 0 && len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    cmd_t c = { .cmd = b[0], .id = (len >= 3) ? (uint16_t)(b[1] | (b[2] << 8)) : 0 };
    if (xQueueSend(s_cmd_q, &c, 0) != pdTRUE) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t n = count_photos();
    uint8_t v[3] = { s_shots_left, n & 0xFF, n >> 8 };
    return os_mbuf_append(ctxt->om, v, sizeof(v)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &ctl_uuid.u,    .access_cb = ctl_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &data_uuid.u,   .access_cb = data_access, .val_handle = &s_data_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &status_uuid.u, .access_cb = status_access, .val_handle = &s_status_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

// ---- GAP -------------------------------------------------------------------------------
static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; ESP_LOGI(TAG, "phone connected"); }
        else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_data_sub = false;
        ESP_LOGI(TAG, "phone disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_data_handle) s_data_sub = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields rsp = {0};
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start failed: %d", rc);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as Pic-Cattcha");
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_cattcha_set_shots_left(uint8_t shots_left)
{
    s_shots_left = shots_left;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint16_t n = count_photos();
    uint8_t v[3] = { shots_left, n & 0xFF, n >> 8 };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(v, sizeof(v));
    if (om) ble_gatts_notify_custom(s_conn, s_status_handle, om);
}

void ble_cattcha_start(uint8_t (*on_new_roll)(void))
{
    s_on_new_roll = on_new_roll;
    s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(transfer_task, "ble_xfer", 6144, NULL, 5, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set("Pic-Cattcha");
    nimble_port_freertos_init(host_task);
}

bool ble_cattcha_is_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
