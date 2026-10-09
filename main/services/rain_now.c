#include "rain_now.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/fuel.h"        // fuel_distance_km
#include "services/location.h"
#include "services/map_tiles.h"
#include "services/net_worker.h"
#include "ui/ui.h"                // ui_lock: swap buffers atomically for the renderer

static const char *TAG = "rain_now";

#define W            RAIN_NOW_W
#define H            RAIN_NOW_H
#define MAP_Z        RAIN_NOW_ZOOM
#define RADAR_Z      7
#define SHIFT        (MAP_Z - RADAR_Z)   // radar pixel = 2^SHIFT map pixels
#define RW           ((W >> SHIFT) + 1)
#define RH           ((H >> SHIFT) + 1)
#define REFRESH_MS   (5 * 60 * 1000)     // RainViewer publishes a frame every 10 min
#define MOVE_KM      3.0f
#define ESRI_URL     "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/"
#define MAX_LISTENERS 2

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    rain_now_cb_t cb;
    void *ctx;
} listener_t;

static listener_t s_listeners[MAX_LISTENERS];
static volatile bool s_paused;
static volatile bool s_kick;
static bool s_have_base;
static uint16_t *s_base;         // basemap for the current origin
static uint16_t *s_img[2];       // composited, double-buffered
static int s_front = -1;         // index shown, -1 = none yet
static int32_t s_ox, s_oy;
static time_t s_radar_time;

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    uint32_t a1 = a + 1, na = 256 - a1;
    uint32_t r = ((src >> 11) * a1 + (dst >> 11) * na) >> 8;
    uint32_t g = (((src >> 5) & 0x3F) * a1 + ((dst >> 5) & 0x3F) * na) >> 8;
    uint32_t b = ((src & 0x1F) * a1 + (dst & 0x1F) * na) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void build_base(http_session_t *http, int32_t ox, int32_t oy)
{
    for (int i = 0; i < W * H; i++) {
        s_base[i] = RGB565(52, 52, 54);
    }
    for (int ty = oy / 256; ty <= (oy + H - 1) / 256; ty++) {
        for (int tx = ox / 256; tx <= (ox + W - 1) / 256; tx++) {
            char url[160], name[24];
            char *data = NULL;
            size_t len = 0;
            uint16_t *rgb = NULL;
            uint8_t *lab = NULL;
            int w = 0, h = 0, lw = 0, lh = 0;
            snprintf(url, sizeof(url), ESRI_URL "World_Dark_Gray_Base/MapServer/tile/%d/%d/%d", MAP_Z, ty, tx);
            snprintf(name, sizeof(name), "b%d_%d_%d", MAP_Z, tx, ty);  // shared with the Radar app's cache
            if (map_tiles_fetch(http, url, name, &data, &len) == ESP_OK) {
                map_tiles_decode_jpeg_rgb565(data, len, &rgb, &w, &h);
                free(data);
            }
            snprintf(url, sizeof(url), ESRI_URL "World_Dark_Gray_Reference/MapServer/tile/%d/%d/%d", MAP_Z, ty, tx);
            snprintf(name, sizeof(name), "r%d_%d_%d", MAP_Z, tx, ty);
            if (map_tiles_fetch(http, url, name, &data, &len) == ESP_OK) {
                map_tiles_decode_rgba(data, len, &lab, &lw, &lh);
                free(data);
            }
            if (rgb) {
                const int x0 = tx * 256 - ox, y0 = ty * 256 - oy;
                for (int y = LV_MAX(0, -y0); y < h && y0 + y < H; y++) {
                    const uint8_t *lr = (lab && lw == w && y < lh) ? lab + (size_t)y * lw * 4 : NULL;
                    for (int x = LV_MAX(0, -x0); x < w && x0 + x < W; x++) {
                        uint16_t px = rgb[(size_t)y * w + x];
                        if (lr && lr[x * 4 + 3]) {
                            const uint8_t *l = lr + x * 4;
                            px = blend565(px, RGB565(l[0], l[1], l[2]), l[3]);
                        }
                        s_base[(size_t)(y0 + y) * W + x0 + x] = px;
                    }
                }
            }
            heap_caps_free(rgb);
            free(lab);
            vTaskDelay(1);
        }
    }
}

// Composes base + latest radar frame into the back buffer. Returns false on network failure.
// Copies in row chunks with short pauses: the RGB panel scans out of the same PSRAM, and a
// long uninterrupted burst starves its refill (visible as the picture shifting/glitching).
static void copy_gently(uint16_t *dst, const uint16_t *src)
{
    for (int y = 0; y < H; y += 64) {
        const int rows = y + 64 <= H ? 64 : H - y;
        memcpy(dst + (size_t)y * W, src + (size_t)y * W, (size_t)rows * W * sizeof(uint16_t));
        vTaskDelay(1);
    }
}

static bool compose(http_session_t *http, uint16_t *out)
{
    copy_gently(out, s_base);
    char *body = NULL;
    if (http_session_get(http, "https://api.rainviewer.com/public/weather-maps.json", 32 * 1024, &body, NULL, NULL) != ESP_OK) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(root, "host"));
    const cJSON *past = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "radar"), "past");
    const cJSON *last = cJSON_GetArrayItem(past, cJSON_GetArraySize(past) - 1);
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(last, "path"));
    if (!host || !path) {
        cJSON_Delete(root);
        return false;
    }
    s_radar_time = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(last, "time"));
    const int32_t rx0 = s_ox >> SHIFT, ry0 = s_oy >> SHIFT;
    for (int ty = ry0 / 256; ty <= (ry0 + RH - 1) / 256; ty++) {
        for (int tx = rx0 / 256; tx <= (rx0 + RW - 1) / 256; tx++) {
            char url[160];
            snprintf(url, sizeof(url), "%s%s/256/%d/%d/%d/2/1_1.png", host, path, RADAR_Z, tx, ty);
            char *png = NULL;
            size_t len = 0;
            uint8_t *rgba = NULL;
            int w, h;
            if (map_tiles_fetch(http, url, NULL, &png, &len) != ESP_OK) {
                continue;
            }
            esp_err_t err = map_tiles_decode_rgba(png, len, &rgba, &w, &h);
            free(png);
            if (err != ESP_OK) {
                continue;
            }
            // Each radar pixel covers 2^SHIFT x 2^SHIFT map pixels; only visit this tile's area.
            const int y0 = LV_MAX(0, ((ty * 256) << SHIFT) - s_oy);
            const int y1 = LV_MIN(H, (((ty * 256 + h) << SHIFT) - s_oy));
            const int x0 = LV_MAX(0, ((tx * 256) << SHIFT) - s_ox);
            const int x1 = LV_MIN(W, (((tx * 256 + w) << SHIFT) - s_ox));
            for (int y = y0; y < y1; y++) {
                if ((y - y0) % 64 == 63) {
                    vTaskDelay(1);  // see copy_gently()
                }
                const int ry = ((s_oy + y) >> SHIFT) - ty * 256;
                if (ry < 0 || ry >= h) {
                    continue;
                }
                for (int x = x0; x < x1; x++) {
                    const int rx = ((s_ox + x) >> SHIFT) - tx * 256;
                    if (rx < 0 || rx >= w) {
                        continue;
                    }
                    const uint8_t *p = rgba + ((size_t)ry * w + rx) * 4;
                    if (p[3] > 8) {
                        out[(size_t)y * W + x] = blend565(out[(size_t)y * W + x], RGB565(p[0], p[1], p[2]), p[3]);
                    }
                }
            }
            free(rgba);
        }
    }
    cJSON_Delete(root);
    return true;
}

static void notify(void)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(s_listeners[i].ctx);
        }
    }
}

static bool alloc_buffers(void)
{
    if (s_base) {
        return true;
    }
    s_base = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_img[0] = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_img[1] = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_base || !s_img[0] || !s_img[1]) {
        heap_caps_free(s_base);
        heap_caps_free(s_img[0]);
        heap_caps_free(s_img[1]);
        s_base = s_img[0] = s_img[1] = NULL;
        ESP_LOGW(TAG, "no PSRAM for the rain map yet");
        return false;
    }
    return true;
}

// Called by the network worker about once a second.
static void rain_now_step(void)
{
    static float base_lat, base_lon;
    static TickType_t last;
    static bool tried;
    location_t loc;
    if (s_paused || wifi_mgr_state() != WIFI_MGR_CONNECTED || !location_get(&loc)) {
        return;
    }
    const bool moved = s_have_base && fuel_distance_km(loc.lat, loc.lon, base_lat, base_lon) > MOVE_KM;
    const bool due = !tried || s_kick || xTaskGetTickCount() - last >= pdMS_TO_TICKS(REFRESH_MS);
    if (!moved && !due && s_have_base) {
        return;
    }
    if (!alloc_buffers()) {
        return;
    }
    s_kick = false;
    tried = true;
    http_session_t http = { 0 };
    map_tiles_cache_init();
    if (!s_have_base || moved) {
        s_ox = (int32_t)lround(mt_lon_to_px(loc.lon, MAP_Z)) - W / 2;
        s_oy = (int32_t)lround(mt_lat_to_px(loc.lat, MAP_Z)) - H / 2;
        build_base(&http, s_ox, s_oy);
        base_lat = loc.lat;
        base_lon = loc.lon;
        s_have_base = true;
    }
    const int back = s_front == 0 ? 1 : 0;
    const bool ok = compose(&http, s_img[back]);
    http_session_close(&http);
    last = xTaskGetTickCount();
    if (ui_lock()) {
        s_front = back;   // swap: the renderer only reads the front buffer under the lock
        ui_unlock();
    }
    ESP_LOGI(TAG, "rain map updated (%s)", ok ? "radar ok" : "radar unavailable, base only");
    notify();
}

void rain_now_init(void)
{
    net_worker_register_step(rain_now_step);  // buffers are allocated on the first step
}

void rain_now_pause(void)
{
    s_paused = true;
}

void rain_now_release(void)
{
    if (!s_base) {
        return;
    }
    if (ui_lock()) {
        s_front = -1;  // the home screen sees "no image" from now on
        ui_unlock();
    }
    heap_caps_free(s_base);
    heap_caps_free(s_img[0]);
    heap_caps_free(s_img[1]);
    s_base = s_img[0] = s_img[1] = NULL;
    s_have_base = false;
    ESP_LOGI(TAG, "buffers released for the Radar");
}

void rain_now_resume(void)
{
    s_paused = false;
    s_kick = true;
}

void rain_now_add_listener(rain_now_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            return;
        }
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i] = (listener_t){ cb, ctx };
            return;
        }
    }
}

void rain_now_remove_listener(rain_now_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            s_listeners[i] = (listener_t){ 0 };
        }
    }
}

const uint16_t *rain_now_image(time_t *radar_time)
{
    if (radar_time) {
        *radar_time = s_radar_time;
    }
    return s_front >= 0 ? s_img[s_front] : NULL;
}

void rain_now_origin(int32_t *ox, int32_t *oy)
{
    *ox = s_ox;
    *oy = s_oy;
}
