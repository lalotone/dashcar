// Weather radar (part of the Weather app): dark map centred on the forecast location with
//  - the last ~2 hours of real radar (RainViewer, zoom 7 max on the free tier), and
//  - the next 12 hours of forecast rain + clouds (Open-Meteo, sampled on a grid and interpolated),
// on one timeline that can be scrubbed or played.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/map_tiles.h"
#include "services/net_worker.h"
#include "services/rain_now.h"
#include "services/traffic.h"
#include "services/weather.h"
#include "esp_attr.h"
#include "services/location.h"
#include "ui.h"

static const char *TAG = "radar";

#define MAP_W       1024
#define MAP_H       (600 - UI_HEADER_H)
#define MAP_Z       8                   // base map zoom
#define RADAR_Z     7                   // RainViewer free-tier max; shown upscaled 2x
#define RADAR_W     (MAP_W / 2 + 1)
#define RADAR_H     (MAP_H / 2 + 1)
#define MAX_PAST    7                   // radar frames, 20 min apart
#define FC_HOURS    12                  // forecast frames (+1h .. +12h)
#define GRID_COLS   12
#define GRID_ROWS   7
#define RAIN_LUT_N  500                 // rain colour LUT, 0.05 mm steps up to 25 mm/h
#define PLAY_MS     800

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint16_t rgb;
    uint8_t  a;
} pal_t;

typedef struct {
    int refs;                     // UI + loader task; freed at 0 (changed under the LVGL lock)
    bool cancel;                  // screen closed
    int32_t utc_offset;
    int ox, oy;                   // world pixel (MAP_Z) of the map's top-left corner

    uint16_t *base;               // RGB565 base map
    uint16_t *out;                // composited frame shown on the canvas

    // Radar frames are downloaded newest-first into slots MAX_PAST-1, MAX_PAST-2, ...
    // so the loaded ones are always the contiguous window [past_first, MAX_PAST).
    int past_first;
    int past_count;               // MAX_PAST - past_first
    time_t past_time[MAX_PAST];
    uint8_t *past[MAX_PAST];      // palette indices, RADAR_W x RADAR_H, 0 = transparent
    pal_t palette[256];
    uint32_t pal_key[256];
    int palette_n;

    bool fc_ready;
    int fc_count;                 // forecast frames available (excludes the current hour)
    time_t fc_time[FC_HOURS + 1]; // [0] = current hour
    uint8_t cloud[FC_HOURS + 1][GRID_ROWS][GRID_COLS];      // %
    uint16_t rain[FC_HOURS + 1][GRID_ROWS][GRID_COLS];      // 0.05 mm units
} radar_ctx_t;

static radar_ctx_t *s_ctx;
static lv_obj_t *s_canvas;
static lv_obj_t *s_slider;
static lv_obj_t *s_time_label;
static lv_obj_t *s_kind_label;
static lv_obj_t *s_status;        // panel
static lv_obj_t *s_status_label;
static lv_obj_t *s_play_label;
static lv_obj_t *s_rain_btn;
static lv_obj_t *s_cloud_btn;
static lv_timer_t *s_play_timer;
static int s_frame;
static bool s_show_rain = true;
static bool s_show_clouds = true;
static pal_t s_rain_lut[RAIN_LUT_N];

// ---------------------------------------------------------------------------
// Context lifetime

// Frees the big buffers (~2.3 MB). Called as soon as the screen closes, under the LVGL lock;
// the loader checks c->cancel under the same lock before touching them.
static void ctx_free_buffers(radar_ctx_t *c)
{
    heap_caps_free(c->base);
    heap_caps_free(c->out);
    c->base = c->out = NULL;
    for (int i = 0; i < MAX_PAST; i++) {
        heap_caps_free(c->past[i]);
        c->past[i] = NULL;
    }
}

static void ctx_release(radar_ctx_t *c)
{
    if (--c->refs > 0) {
        return;
    }
    ctx_free_buffers(c);
    heap_caps_free(c);
}

// ---------------------------------------------------------------------------
// Colours

static void build_rain_lut(void)
{
    static const struct { float mm; uint8_t r, g, b, a; } stops[] = {
        { 0.1f, 133, 197, 255, 90 },
        { 0.5f, 54, 186, 229, 170 },
        { 2.0f, 0, 163, 224, 210 },
        { 5.0f, 255, 170, 0, 220 },
        { 10.0f, 255, 129, 0, 230 },
        { 25.0f, 143, 0, 0, 240 },
    };
    const int n = sizeof(stops) / sizeof(stops[0]);
    for (int i = 0; i < RAIN_LUT_N; i++) {
        float mm = i * 0.05f;
        if (mm < stops[0].mm) {
            s_rain_lut[i] = (pal_t){ 0, 0 };
            continue;
        }
        int k = 0;
        while (k < n - 2 && mm > stops[k + 1].mm) {
            k++;
        }
        float t = fminf(1.0f, (mm - stops[k].mm) / (stops[k + 1].mm - stops[k].mm));
#define MIX(f) (uint8_t)(stops[k].f + t * (stops[k + 1].f - stops[k].f))
        s_rain_lut[i] = (pal_t){ RGB565(MIX(r), MIX(g), MIX(b)), MIX(a) };
#undef MIX
    }
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    uint32_t dr = dst >> 11, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    uint32_t sr = src >> 11, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    uint32_t a1 = a + 1, na = 256 - a1;
    uint32_t r = (sr * a1 + dr * na) >> 8;
    uint32_t g = (sg * a1 + dg * na) >> 8;
    uint32_t b = (sb * a1 + db * na) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Palette index for an RGBA radar pixel (shared by all frames; ~100 colours in practice).
static uint8_t palette_index(radar_ctx_t *c, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 8) {
        return 0;
    }
    uint32_t key = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
    for (int i = 1; i <= c->palette_n; i++) {
        if (c->pal_key[i] == key) {
            return i;
        }
    }
    if (c->palette_n < 255) {
        int i = ++c->palette_n;
        c->pal_key[i] = key;
        c->palette[i] = (pal_t){ RGB565(r, g, b), a };
        return i;
    }
    // Palette full: nearest colour.
    int best = 1, best_d = INT32_MAX;
    for (int i = 1; i <= 255; i++) {
        uint32_t k = c->pal_key[i];
        int dr = (int)(k >> 24) - r, dg = (int)((k >> 16) & 0xFF) - g;
        int db = (int)((k >> 8) & 0xFF) - b, da = (int)(k & 0xFF) - a;
        int d = dr * dr + dg * dg + db * db + da * da;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Composition (LVGL task)

static int frame_count(const radar_ctx_t *c)
{
    return c->past_count + (c->fc_ready ? c->fc_count : 0);
}

// Interpolated cloud (%) and rain (LUT index) fields for forecast hour k, drawn over `out`.
static void draw_forecast(radar_ctx_t *c, int k, bool rain, bool clouds)
{
    static uint16_t xi[MAP_W];
    static uint16_t xt[MAP_W];  // 0..256
    for (int x = 0; x < MAP_W; x++) {
        uint32_t fx = (uint32_t)x * (GRID_COLS - 1) * 256 / (MAP_W - 1);
        xi[x] = LV_MIN(fx >> 8, GRID_COLS - 2);
        xt[x] = fx - xi[x] * 256;
    }
    int32_t row_cloud[GRID_COLS], row_rain[GRID_COLS];
    for (int y = 0; y < MAP_H; y++) {
        if (y % 64 == 63) {
            vTaskDelay(1);
        }
        uint32_t fy = (uint32_t)y * (GRID_ROWS - 1) * 256 / (MAP_H - 1);
        int j = LV_MIN(fy >> 8, GRID_ROWS - 2);
        int ty = fy - j * 256;
        for (int i = 0; i < GRID_COLS; i++) {
            row_cloud[i] = (c->cloud[k][j][i] * (256 - ty) + c->cloud[k][j + 1][i] * ty);        // % * 256
            row_rain[i] = (c->rain[k][j][i] * (256 - ty) + c->rain[k][j + 1][i] * ty);           // lut * 256
        }
        uint16_t *px = c->out + y * MAP_W;
        for (int x = 0; x < MAP_W; x++) {
            int i = xi[x], t = xt[x];
            if (clouds) {
                int32_t cl = (row_cloud[i] * (256 - t) + row_cloud[i + 1] * t) >> 16;  // %
                if (cl > 30) {
                    // Subtle: only real cloud decks show, and labels stay readable.
                    int a = (cl - 30) * 120 / 70;
                    px[x] = blend565(px[x], RGB565(200, 210, 225), a);
                }
            }
            if (rain) {
                int32_t r = (row_rain[i] * (256 - t) + row_rain[i + 1] * t) >> 16;
                if (r > 0) {
                    const pal_t *p = &s_rain_lut[LV_MIN(r, RAIN_LUT_N - 1)];
                    if (p->a) {
                        px[x] = blend565(px[x], p->rgb, p->a);
                    }
                }
            }
        }
    }
}

static void draw_radar(radar_ctx_t *c, int f)
{
    const uint8_t *frame = c->past[c->past_first + f];
    if (!frame) {
        return;
    }
    const int rx0 = c->ox >> 1, ry0 = c->oy >> 1;
    for (int y = 0; y < MAP_H; y++) {
        if (y % 64 == 63) {
            vTaskDelay(1);
        }
        const uint8_t *row = frame + (((c->oy + y) >> 1) - ry0) * RADAR_W;
        uint16_t *px = c->out + y * MAP_W;
        for (int x = 0; x < MAP_W; x++) {
            uint8_t idx = row[((c->ox + x) >> 1) - rx0];
            if (idx) {
                const pal_t *p = &c->palette[idx];
                px[x] = blend565(px[x], p->rgb, p->a);
            }
        }
    }
}

static void update_labels(radar_ctx_t *c)
{
    char buf[48], t[16];
    int n = frame_count(c);
    if (n == 0) {
        lv_label_set_text(s_time_label, "--:--");
        lv_label_set_text(s_kind_label, "");
        return;
    }
    if (s_frame < c->past_count) {
        time_t ft = c->past_time[c->past_first + s_frame];
        ui_fmt_local(t, sizeof(t), "%H:%M", ft, c->utc_offset);
        int mins = (int)((time(NULL) - ft) / 60);
        if (s_frame == c->past_count - 1 || mins < 10) {
            snprintf(buf, sizeof(buf), "%s  ·  Now", t);
        } else {
            snprintf(buf, sizeof(buf), "%s  ·  %d min ago", t, mins);
        }
        lv_label_set_text(s_kind_label, "RADAR");
        lv_obj_set_style_text_color(s_kind_label, lv_color_hex(0x38BDF8), 0);
    } else {
        int k = s_frame - c->past_count + 1;
        ui_fmt_local(t, sizeof(t), "%H:%M", c->fc_time[k], c->utc_offset);
        snprintf(buf, sizeof(buf), "%s  ·  +%d h", t, k);
        lv_label_set_text(s_kind_label, "FORECAST");
        lv_obj_set_style_text_color(s_kind_label, UI_COLOR_WARN, 0);
    }
    lv_label_set_text(s_time_label, buf);
}

static void render(void)
{
    radar_ctx_t *c = s_ctx;
    if (!c || !c->out) {
        return;  // buffers are allocated by the loader job
    }
    // Row chunks with short pauses: the panel scans out of the same PSRAM (see ESP32-info §6).
    for (int y = 0; y < MAP_H; y += 64) {
        const int rows = y + 64 <= MAP_H ? 64 : MAP_H - y;
        memcpy(c->out + (size_t)y * MAP_W, c->base + (size_t)y * MAP_W, (size_t)rows * MAP_W * sizeof(uint16_t));
        vTaskDelay(1);
    }
    int n = frame_count(c);
    if (n > 0) {
        s_frame = LV_CLAMP(0, s_frame, n - 1);
        if (s_frame < c->past_count) {
            // Radar shows observed rain; clouds come from the current forecast hour.
            if (s_show_clouds && c->fc_ready) {
                draw_forecast(c, 0, false, true);
            }
            if (s_show_rain) {
                draw_radar(c, s_frame);
            }
        } else {
            draw_forecast(c, s_frame - c->past_count + 1, s_show_rain, s_show_clouds);
        }
    }
    update_labels(c);
    lv_obj_invalidate(s_canvas);
}

static void timeline_update(void)
{
    radar_ctx_t *c = s_ctx;
    int n = frame_count(c);
    if (n <= 1) {
        lv_obj_add_state(s_slider, LV_STATE_DISABLED);
        lv_slider_set_range(s_slider, 0, 1);
        return;
    }
    lv_obj_remove_state(s_slider, LV_STATE_DISABLED);
    lv_slider_set_range(s_slider, 0, n - 1);
    lv_slider_set_value(s_slider, s_frame, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// Loader task: base map -> forecast grid -> radar frames (newest first)

static void set_status(radar_ctx_t *c, const char *text)
{
    if (ui_lock()) {
        if (!c->cancel) {
            if (text) {
                lv_label_set_text(s_status_label, text);
                lv_obj_set_hidden(s_status, false);
            } else {
                lv_obj_set_hidden(s_status, true);
            }
        }
        ui_unlock();
    }
}

// Esri "Dark Gray Canvas": JPEG base + transparent PNG labels/boundaries on top.
#define ESRI_URL "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/"

#define BASE_RENDER_EVERY 4   // recompose the map every N tiles, not after each one

static void load_base(radar_ctx_t *c, http_session_t *http)
{
    int tx0 = c->ox / 256, tx1 = (c->ox + MAP_W - 1) / 256;
    int ty0 = c->oy / 256, ty1 = (c->oy + MAP_H - 1) / 256;
    int done = 0;
    for (int ty = ty0; ty <= ty1 && !c->cancel; ty++) {
        for (int tx = tx0; tx <= tx1 && !c->cancel; tx++) {
            char url[160], name[24];
            char *data = NULL;
            size_t len = 0;
            uint16_t *rgb = NULL;
            uint8_t *labels = NULL;
            int w = 0, h = 0, lw = 0, lh = 0;

            snprintf(url, sizeof(url), ESRI_URL "World_Dark_Gray_Base/MapServer/tile/%d/%d/%d", MAP_Z, ty, tx);
            snprintf(name, sizeof(name), "b%d_%d_%d", MAP_Z, tx, ty);
            if (map_tiles_fetch(http, url, name, &data, &len) == ESP_OK) {
                map_tiles_decode_jpeg_rgb565(data, len, &rgb, &w, &h);
                free(data);
            }
            snprintf(url, sizeof(url), ESRI_URL "World_Dark_Gray_Reference/MapServer/tile/%d/%d/%d", MAP_Z, ty, tx);
            snprintf(name, sizeof(name), "r%d_%d_%d", MAP_Z, tx, ty);
            if (map_tiles_fetch(http, url, name, &data, &len) == ESP_OK) {
                map_tiles_decode_rgba(data, len, &labels, &lw, &lh);
                free(data);
            }
            if (rgb && ui_lock()) {
                if (c->cancel) {  // screen closed: buffers are gone
                    ui_unlock();
                    heap_caps_free(rgb);
                    free(labels);
                    break;
                }
                int x0 = tx * 256 - c->ox, y0 = ty * 256 - c->oy;
                for (int y = LV_MAX(0, -y0); y < h && y0 + y < MAP_H; y++) {
                    uint16_t *dst = c->base + (size_t)(y0 + y) * MAP_W;
                    const uint16_t *src = rgb + (size_t)y * w;
                    const uint8_t *lab = (labels && lw == w && y < lh) ? labels + (size_t)y * lw * 4 : NULL;
                    for (int x = LV_MAX(0, -x0); x < w && x0 + x < MAP_W; x++) {
                        uint16_t px = src[x];
                        if (lab && lab[x * 4 + 3]) {
                            const uint8_t *l = lab + x * 4;
                            px = blend565(px, RGB565(l[0], l[1], l[2]), l[3]);
                        }
                        dst[x0 + x] = px;
                    }
                }
                // Each full recompose streams ~2 MB through PSRAM, which the RGB panel also
                // reads from; doing it per tile starved the display (drift) and the UI.
                if (++done % BASE_RENDER_EVERY == 0) {
                    render();
                }
                ui_unlock();
            }
            heap_caps_free(rgb);
            free(labels);
            vTaskDelay(1);  // let the LVGL task run between tiles
        }
    }
    if (ui_lock()) {
        if (!c->cancel) {
            render();
        }
        ui_unlock();
    }
}

static bool load_forecast(radar_ctx_t *c, http_session_t *http)
{
    // Grid points sit on the map's pixel grid so interpolation is a plain bilinear.
    char lats[GRID_ROWS * GRID_COLS * 9 + 1] = "", lons[GRID_ROWS * GRID_COLS * 10 + 1] = "";
    for (int j = 0; j < GRID_ROWS; j++) {
        double lat = mt_px_to_lat(c->oy + (double)j * (MAP_H - 1) / (GRID_ROWS - 1), MAP_Z);
        for (int i = 0; i < GRID_COLS; i++) {
            double lon = mt_px_to_lon(c->ox + (double)i * (MAP_W - 1) / (GRID_COLS - 1), MAP_Z);
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%s%.3f", lats[0] ? "," : "", lat);
            strlcat(lats, tmp, sizeof(lats));
            snprintf(tmp, sizeof(tmp), "%s%.3f", lons[0] ? "," : "", lon);
            strlcat(lons, tmp, sizeof(lons));
        }
    }
    char *url = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!url) {
        return false;
    }
    snprintf(url, 2048,
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&hourly=cloud_cover,precipitation&forecast_hours=%d&timeformat=unixtime",
             lats, lons, FC_HOURS + 1);
    char *body = NULL;
    esp_err_t err = http_session_get(http, url, 128 * 1024, &body, NULL, NULL);
    free(url);
    if (err != ESP_OK) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) != GRID_ROWS * GRID_COLS) {
        ESP_LOGW(TAG, "unexpected forecast grid response");
        cJSON_Delete(root);
        return false;
    }

    bool ok = ui_lock();
    if (ok) {
        int hours = FC_HOURS + 1;
        for (int p = 0; p < GRID_ROWS * GRID_COLS; p++) {
            const cJSON *hourly = cJSON_GetObjectItem(cJSON_GetArrayItem(root, p), "hourly");
            const cJSON *t = cJSON_GetObjectItem(hourly, "time");
            const cJSON *cc = cJSON_GetObjectItem(hourly, "cloud_cover");
            const cJSON *pr = cJSON_GetObjectItem(hourly, "precipitation");
            int n = LV_MIN(cJSON_GetArraySize(t), FC_HOURS + 1);
            hours = LV_MIN(hours, n);
            for (int k = 0; k < n; k++) {
                if (p == 0) {
                    c->fc_time[k] = (time_t)cJSON_GetArrayItem(t, k)->valuedouble;
                }
                const cJSON *cv = cJSON_GetArrayItem(cc, k);
                const cJSON *pv = cJSON_GetArrayItem(pr, k);
                c->cloud[k][p / GRID_COLS][p % GRID_COLS] = cJSON_IsNumber(cv) ? (uint8_t)cv->valuedouble : 0;
                float mm = cJSON_IsNumber(pv) ? (float)pv->valuedouble : 0;
                c->rain[k][p / GRID_COLS][p % GRID_COLS] = (uint16_t)LV_MIN(mm / 0.05f, RAIN_LUT_N - 1);
            }
        }
        c->fc_count = LV_MAX(0, hours - 1);
        c->fc_ready = c->fc_count > 0;
        if (!c->cancel) {
            if (c->past_count == 0) {
                s_frame = 0;  // no radar yet: start at +1h
            }
            timeline_update();
            render();
        }
        ui_unlock();
    }
    cJSON_Delete(root);
    return ok;
}

static bool load_radar_frame(radar_ctx_t *c, http_session_t *http, const char *host, const char *path, uint8_t *frame)
{
    const int rx0 = c->ox >> 1, ry0 = c->oy >> 1;
    int tx0 = rx0 / 256, tx1 = (rx0 + RADAR_W - 1) / 256;
    int ty0 = ry0 / 256, ty1 = (ry0 + RADAR_H - 1) / 256;
    bool any = false;
    for (int ty = ty0; ty <= ty1 && !c->cancel; ty++) {
        for (int tx = tx0; tx <= tx1 && !c->cancel; tx++) {
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
            if (ui_lock()) {  // the palette is shared with the renderer
                if (c->cancel) {
                    ui_unlock();
                    free(rgba);
                    return false;
                }
                int x0 = tx * 256 - rx0, y0 = ty * 256 - ry0;
                for (int y = LV_MAX(0, -y0); y < h && y0 + y < RADAR_H; y++) {
                    const uint8_t *src = rgba + (size_t)y * w * 4;
                    uint8_t *dst = frame + (size_t)(y0 + y) * RADAR_W;
                    for (int x = LV_MAX(0, -x0); x < w && x0 + x < RADAR_W; x++) {
                        const uint8_t *p = src + x * 4;
                        dst[x0 + x] = palette_index(c, p[0], p[1], p[2], p[3]);
                    }
                }
                ui_unlock();
                any = true;
            }
            free(rgba);
        }
    }
    return any;
}

static void load_radar(radar_ctx_t *c, http_session_t *http)
{
    char *body = NULL;
    if (http_session_get(http, "https://api.rainviewer.com/public/weather-maps.json", 32 * 1024, &body, NULL, NULL) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItem(root, "host"));
    const cJSON *past = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "radar"), "past");
    int total = cJSON_GetArraySize(past);
    if (!host || total == 0) {
        cJSON_Delete(root);
        return;
    }
    // Every other frame (20 min apart), oldest first, ending with the newest.
    int idx[MAX_PAST], n = 0;
    for (int i = total - 1; i >= 0 && n < MAX_PAST; i -= 2) {
        idx[n++] = i;
    }
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        int t = idx[a];
        idx[a] = idx[b];
        idx[b] = t;
    }

    // Download newest first so "now" appears quickly, then extend the timeline backwards.
    for (int f = n - 1; f >= 0 && !c->cancel; f--) {
        const cJSON *item = cJSON_GetArrayItem(past, idx[f]);
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(item, "path"));
        if (!path) {
            break;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "Loading radar %d/%d...", n - f, n);
        set_status(c, msg);
        uint8_t *frame = heap_caps_calloc(RADAR_W * RADAR_H, 1, MALLOC_CAP_SPIRAM);
        if (!frame) {
            break;
        }
        if (!load_radar_frame(c, http, host, path, frame)) {
            heap_caps_free(frame);
            break;  // keep the timeline contiguous
        }
        if (!ui_lock()) {
            heap_caps_free(frame);
            break;
        }
        if (c->cancel) {
            ui_unlock();
            heap_caps_free(frame);
            break;
        }
        int slot = MAX_PAST - (n - f);
        c->past[slot] = frame;
        c->past_time[slot] = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "time"));
        bool first = c->past_count == 0;
        c->past_first = slot;
        c->past_count = MAX_PAST - slot;
        if (!c->cancel) {
            // An older frame was prepended: keep the user on the same moment.
            s_frame = first ? 0 : s_frame + 1;
            timeline_update();
            if (first) {
                render();
            } else {
                update_labels(c);
            }
        }
        ui_unlock();
    }
    cJSON_Delete(root);
}

// Runs on the network worker (one job at a time), so releasing the home rain map's buffers
// here can't overlap a rain-map update.
static void loader_job(void *arg)
{
    radar_ctx_t *c = arg;
    http_session_t http = { 0 };

    rain_now_release();
    uint16_t *base = heap_caps_malloc(MAP_W * MAP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    uint16_t *out = heap_caps_malloc(MAP_W * MAP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (ui_lock()) {
        if (!c->cancel && base && out) {
            c->base = base;
            c->out = out;
            base = out = NULL;
            for (int i = 0; i < MAP_W * MAP_H; i++) {
                c->base[i] = RGB565(52, 52, 54);  // close to the Esri dark canvas
            }
            if (s_ctx == c) {
                lv_canvas_set_buffer(s_canvas, c->out, MAP_W, MAP_H, LV_COLOR_FORMAT_RGB565);
                lv_obj_set_hidden(s_canvas, false);
                render();
            }
        } else if (!c->cancel) {
            lv_label_set_text(s_status_label, "Not enough memory for the map");
        }
        const bool stop = c->cancel || !c->base;
        if (stop) {
            ctx_release(c);
        }
        ui_unlock();
        heap_caps_free(base);  // unused (cancelled or out of memory)
        heap_caps_free(out);
        if (stop) {
            return;
        }
    }

    set_status(c, "Loading map...");
    map_tiles_cache_init();
    load_base(c, &http);
    if (!c->cancel) {
        set_status(c, "Loading forecast...");
        if (!load_forecast(c, &http)) {
            ESP_LOGW(TAG, "forecast grid unavailable");
        }
    }
    if (!c->cancel) {
        load_radar(c, &http);
    }
    http_session_close(&http);

    if (ui_lock()) {
        if (!c->cancel) {
            lv_obj_set_hidden(s_status, frame_count(c) > 0);
            if (frame_count(c) == 0) {
                lv_label_set_text(s_status_label, "Couldn't load rain data");
            }
        }
        ctx_release(c);
        ui_unlock();
    }
    ESP_LOGI(TAG, "loader done, PSRAM free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ---------------------------------------------------------------------------
// UI

static void set_playing(bool play)
{
    if (play) {
        lv_timer_resume(s_play_timer);
        lv_label_set_text(s_play_label, LV_SYMBOL_PAUSE);
    } else {
        lv_timer_pause(s_play_timer);
        lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
    }
}

static void on_play_tick(lv_timer_t *t)
{
    int n = s_ctx ? frame_count(s_ctx) : 0;
    if (n <= 1) {
        return;
    }
    s_frame = (s_frame + 1) % n;
    lv_slider_set_value(s_slider, s_frame, LV_ANIM_OFF);
    render();
}

static void on_play(lv_event_t *e)
{
    set_playing(lv_timer_get_paused(s_play_timer));
}

static void on_slider(lv_event_t *e)
{
    int v = lv_slider_get_value(s_slider);
    if (v != s_frame) {
        s_frame = v;
        set_playing(false);
        render();
    }
}

static void style_toggle(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_HI, 0);
}

static void on_toggle(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    if (btn == s_rain_btn) {
        s_show_rain = !s_show_rain;
        style_toggle(btn, s_show_rain);
    } else {
        s_show_clouds = !s_show_clouds;
        style_toggle(btn, s_show_clouds);
    }
    render();
}

static void on_back(lv_event_t *e)
{
    ui_weather_open();
}

static void on_delete(lv_event_t *e)
{
    // This instance's own context (another radar screen may already be open).
    radar_ctx_t *c = lv_event_get_user_data(e);
    c->cancel = true;
    ctx_free_buffers(c);  // give the 2.3 MB back now, not when the loader's download ends
    if (s_ctx == c) {
        s_ctx = NULL;
        s_play_timer = NULL;  // deleted with the screen (ui_screen_own_timer)
        rain_now_resume();    // no newer radar instance: give the home rain map its memory back
    }
    ctx_release(c);
}

static lv_obj_t *overlay_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_scrollable(p, false);
    return p;
}

void ui_radar_open(void)
{
    location_t loc;
    if (!location_get(&loc)) {
        ui_toast("Waiting for your location");
        return;
    }
    radar_ctx_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM);  // buffers: loader job
    if (!c) {
        ui_toast("Not enough memory for the map");
        return;
    }
    static bool lut_ready;
    if (!lut_ready) {
        build_rain_lut();
        lut_ready = true;
    }

    c->refs = 2;  // this screen + loader task
    c->utc_offset = weather_utc_offset();
    c->ox = (int)lround(mt_lon_to_px(loc.lon, MAP_Z)) - MAP_W / 2;
    c->oy = (int)lround(mt_lat_to_px(loc.lat, MAP_Z)) - MAP_H / 2;
    s_ctx = c;
    s_frame = 0;

    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, c);

    char title[128];
    snprintf(title, sizeof(title), "Radar  ·  %s", loc.name);
    lv_obj_t *hdr = ui_header_create(scr, title, on_back);
    s_rain_btn = ui_header_button(hdr, NULL, "Rain", on_toggle, NULL);
    s_cloud_btn = ui_header_button(hdr, NULL, "Clouds", on_toggle, NULL);
    style_toggle(s_rain_btn, s_show_rain);
    style_toggle(s_cloud_btn, s_show_clouds);

    s_canvas = lv_canvas_create(scr);
    lv_obj_set_pos(s_canvas, 0, UI_HEADER_H);
    lv_obj_set_hidden(s_canvas, true);  // until the loader job has the buffers

    // Major road incidents (DGT) as small dots under the you-are-here marker.
    EXT_RAM_BSS_ATTR static traffic_item_t inc[40];
    int ninc = traffic_get(inc, 40, 400, TRAFFIC_MEDIUM, NULL);
    for (int i = 0; i < ninc; i++) {
        int32_t px = (int32_t)lround(mt_lon_to_px(inc[i].lon, MAP_Z)) - c->ox;
        int32_t py = (int32_t)lround(mt_lat_to_px(inc[i].lat, MAP_Z)) - c->oy;
        if (px < 6 || py < 6 || px > MAP_W - 6 || py > MAP_H - 6) {
            continue;
        }
        lv_obj_t *m = lv_obj_create(scr);
        lv_obj_remove_style_all(m);
        lv_obj_set_size(m, 12, 12);
        lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(m, ui_traffic_color(inc[i].sev), 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(m, lv_color_black(), 0);
        lv_obj_set_style_border_width(m, 2, 0);
        lv_obj_set_clickable(m, false);
        lv_obj_set_pos(m, px - 6, UI_HEADER_H + py - 6);
    }

    // You-are-here marker
    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 18, 18);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_set_style_border_width(dot, 3, 0);
    lv_obj_set_pos(dot, MAP_W / 2 - 9, UI_HEADER_H + MAP_H / 2 - 9);

    // Timeline panel
    lv_obj_t *bar = overlay_panel(scr);
    lv_obj_set_size(bar, MAP_W - 2 * UI_PAD, 84);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -UI_PAD);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 20, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);

    lv_obj_t *play = lv_button_create(bar);
    lv_obj_set_size(play, 56, 56);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(play, on_play, LV_EVENT_CLICKED, NULL);
    s_play_label = lv_label_create(play);
    lv_obj_set_style_text_font(s_play_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_play_label);

    lv_obj_t *labels = lv_obj_create(bar);
    lv_obj_remove_style_all(labels);
    lv_obj_set_size(labels, 230, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(labels, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(labels, false);
    s_kind_label = ui_label_create(labels, ui_font_sm, UI_COLOR_MUTED, "");
    s_time_label = ui_label_create(labels, ui_font_lg, UI_COLOR_TEXT, "--:--");

    s_slider = lv_slider_create(bar);
    lv_obj_set_flex_grow(s_slider, 1);
    lv_obj_set_height(s_slider, 12);
    lv_obj_set_style_pad_right(s_slider, 12, 0);
    lv_obj_set_ext_click_area(s_slider, 24);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *attrib = ui_label_create(scr, ui_font_sm, UI_COLOR_MUTED,
                                       "Map: Esri, HERE, Garmin, © OpenStreetMap  ·  Radar: RainViewer  ·  Forecast: Open-Meteo");
    lv_obj_align(attrib, LV_ALIGN_BOTTOM_RIGHT, -UI_PAD - 4, -UI_PAD - 88);

    s_status = overlay_panel(scr);
    lv_obj_set_size(s_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_status, 20, 0);
    lv_obj_set_style_pad_ver(s_status, 10, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, UI_HEADER_H + UI_PAD);
    s_status_label = ui_label_create(s_status, ui_font_md, UI_COLOR_TEXT, "Loading map...");

    s_play_timer = lv_timer_create(on_play_tick, PLAY_MS, NULL);
    ui_screen_own_timer(scr, s_play_timer);
    set_playing(false);
    timeline_update();
    render();
    ui_screen_load(scr);

    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        lv_label_set_text(s_status_label, "No Wi-Fi connection");
        s_ctx->refs--;  // no loader
        return;
    }
    rain_now_pause();  // the loader job frees the home rain map's ~1 MB before allocating ours
    if (!net_worker_post(loader_job, c)) {
        s_ctx->refs--;
        rain_now_resume();
        lv_label_set_text(s_status_label, "Busy, please try again");
    }
}

