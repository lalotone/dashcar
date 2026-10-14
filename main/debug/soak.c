// Soak test (tools/soak.sh only, CONFIG_DASHCAR_SOAK): a realistic ~35 min "daily drive" on real Wi-Fi.
//  - drive task: simulated GPS along real roads (debug/soak_route.h): park, A-2 at 100 km/h past a fixed
//    camera, stop (parking save), Z-40 ring road through an average-speed section, stop.
//    Everything that follows the car runs for real: weather/fuel/rain map/alerts refetches, OSM speed
//    limits, camera alerts, trip computer.
//  - UI task: a user tapping through every app (Radar and Fuel included) while all that happens.
//  - monitor task: heap/fragmentation, stack headroom, slowest LVGL refresh, ESP_LOG errors, and a
//    frame-buffer check (the frame on screen must match a fresh render of the screen when idle:
//    catches "glitch lines" that stay on screen).
// Ends with "soak: RESULT: PASS" or "soak: RESULT: FAIL (n)" and the reasons.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board/board.h"
#include "debug/soak.h"
#include "debug/soak_route.h"
#include "debug/touch_sim.h"
#include "net/wifi_mgr.h"
#include "services/fuel.h"
#include "services/gps.h"
#include "services/parking.h"
#include "services/speedlimit.h"
#include "services/trip.h"
#include "settings.h"
#include "ui/ui.h"

#define AT(...) ESP_LOGW("soak", __VA_ARGS__)

// Home card centres (as in selftest.c)
#define CARD_WEATHER 576, 189
#define CARD_RADAR   748, 189
#define CARD_TRAFFIC 920, 189
#define CARD_FUEL    576, 451
#define CARD_GPS     748, 451
#define CARD_TRIP    920, 451
#define CARD_EXPLORE 576, 451   // after scrolling the grid once
#define CARD_CAMERAS 748, 451
#define CARD_PARKING 920, 451
#define CARD_SETTING 576, 451   // after scrolling twice
#define STATUS_BAR   900, 32

static esp_lcd_panel_handle_t s_panel;
static volatile bool s_done;
static volatile bool s_radar_busy;   // UI task is in the Radar: skip the frame-buffer check (1.2 MB snapshot)
static int s_fail;
static night_mode_t s_prev_night;

// ---------------------------------------------------------------------------
// Log hook: counts errors and the events we expect, then prints as usual.

static vprintf_like_t s_prev_vprintf;
static volatile int s_net_errors;
static volatile int s_errors, s_radar_nomem, s_cam_fixed, s_sec_in, s_sec_end, s_roads, s_weather, s_fuel, s_parked;
static char s_first_errors[4][120];

static int soak_vprintf(const char *fmt, va_list ap)
{
    char line[160];
    va_list copy;
    va_copy(copy, ap);
    vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    // Known-harmless driver noise: the Wi-Fi driver drops a duplicated encrypted frame from the phone.
    const bool benign = strstr(line, "CCMP replay detected") != NULL;
    // Network failures (DNS, connect, TLS) happen on a phone hotspot in a car: counted and reported,
    // not failures by themselves (the app must just carry on, which the other checks verify).
    const bool network = strstr(line, "esp-tls") || strstr(line, "transport_base") || strstr(line, "HTTP_CLIENT") ||
                         strstr(line, "getaddrinfo");
    if (line[0] == 'E' && line[1] == ' ' && line[2] == '(' && network) {
        s_net_errors++;
    } else if (line[0] == 'E' && line[1] == ' ' && line[2] == '(' && !benign) {
        if (s_errors < 4) {
            strlcpy(s_first_errors[s_errors], line, sizeof(s_first_errors[0]));
        }
        s_errors++;
    }
    if (strstr(line, "map buffers: no")) s_radar_nomem++;
    if (strstr(line, "fixed camera ahead")) s_cam_fixed++;
    if (strstr(line, "section entered")) s_sec_in++;
    if (strstr(line, "section ended")) s_sec_end++;
    if (strstr(line, "speedlimit: ") && strstr(line, " roads, ")) s_roads++;
    if (strstr(line, "weather: ") && strstr(line, ", code ")) s_weather++;
    if (strstr(line, "fuel: product ")) s_fuel++;
    if (strstr(line, "gps: parked at")) s_parked++;
    return s_prev_vprintf(fmt, ap);
}

static void check(bool ok, const char *what)
{
    if (!ok) {
        s_fail++;
        AT("FAIL: %s", what);
    } else {
        AT("ok: %s", what);
    }
}

// ---------------------------------------------------------------------------
// Drive

static float seg_km(const route_pt_t *a, const route_pt_t *b)
{
    return fuel_distance_km(a->lat, a->lon, b->lat, b->lon);
}

static float bearing(const route_pt_t *a, const route_pt_t *b)
{
    const float r = (float)M_PI / 180, kx = cosf(a->lat * r);
    return fmodf(atan2f((b->lon - a->lon) * kx, b->lat - a->lat) / r + 360, 360);
}

static int s_limit_known, s_limit_samples;

static void park(float lat, float lon, int seconds)
{
    for (int i = 0; i < seconds; i++) {
        gps_sim_feed(lat, lon, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// One fix per second, moving `kmh` along the polyline. Returns the route length (km).
static float drive(const route_pt_t *r, int n, float kmh)
{
    float total = 0;
    for (int i = 0; i + 1 < n; i++) {
        total += seg_km(&r[i], &r[i + 1]);
    }
    const float step = kmh / 3600;  // km per fix
    int i = 0;
    float along = 0;                // km into segment i
    while (i + 1 < n) {
        const float len = seg_km(&r[i], &r[i + 1]);
        if (along > len) {
            along -= len;
            i++;
            continue;
        }
        const float t = len > 0 ? along / len : 0;
        const float lat = r[i].lat + (r[i + 1].lat - r[i].lat) * t, lon = r[i].lon + (r[i + 1].lon - r[i].lon) * t;
        gps_sim_feed(lat, lon, kmh, bearing(&r[i], &r[i + 1]));
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ui_lock()) {
            speedlimit_t sl;
            speedlimit_get(&sl);
            ui_unlock();
            s_limit_samples++;
            s_limit_known += sl.kmh > 0;
        }
        along += step;
    }
    return total;
}

static void drive_task(void *arg)
{
    // Real Wi-Fi is part of the test (downloads compete with the UI).
    for (int i = 0; i < 90 && wifi_mgr_state() != WIFI_MGR_CONNECTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    check(wifi_mgr_state() == WIFI_MGR_CONNECTED, "Wi-Fi connected within 90 s");
    trip_info_t t0;
    trip_get(&t0);

    const int na = sizeof(ROUTE_A2) / sizeof(ROUTE_A2[0]), nz = sizeof(ROUTE_Z40) / sizeof(ROUTE_Z40[0]);
    AT("phase: parked at the start (30 s)");
    park(ROUTE_A2[0].lat, ROUTE_A2[0].lon, 30);
    AT("phase: A-2, 100 km/h");
    float km = drive(ROUTE_A2, na, 100);
    AT("phase: stop at the A-2 end (150 s: parking + trip save)");
    park(ROUTE_A2[na - 1].lat, ROUTE_A2[na - 1].lon, 150);
    parking_spot_t p;
    parking_get(&p);
    check(s_parked >= 1 && !p.demo &&
          fuel_distance_km((float)p.lat, (float)p.lon, ROUTE_A2[na - 1].lat, ROUTE_A2[na - 1].lon) < 0.3f,
          "parking spot saved at the A-2 stop");
    AT("phase: Z-40 (average-speed section), 90 km/h");
    km += drive(ROUTE_Z40, nz, 90);
    AT("phase: stop at the Z-40 end (60 s)");
    park(ROUTE_Z40[nz - 1].lat, ROUTE_Z40[nz - 1].lon, 60);

    trip_info_t t1;
    trip_get(&t1);
    const float trip_km = (float)(t1.total_km - t0.total_km);
    char what[96];
    snprintf(what, sizeof(what), "trip counted %.1f km of %.1f km driven (within 10%%)", trip_km, km);
    check(fabsf(trip_km - km) < 0.1f * km, what);
    snprintf(what, sizeof(what), "speed limit known for %d of %d driving seconds (>= 50%%)", s_limit_known,
             s_limit_samples);
    check(s_limit_known * 2 >= s_limit_samples, what);
    check(s_cam_fixed >= 1, "fixed camera alert on the A-2");
    check(s_sec_in >= 1 && s_sec_end >= 1, "average-speed section entered and ended on the Z-40");
    snprintf(what, sizeof(what), "refetched while moving: speed limits %d (>= 3), weather %d (>= 2), fuel %d (>= 2)",
             s_roads, s_weather, s_fuel);
    check(s_roads >= 3 && s_weather >= 2 && s_fuel >= 2, what);
    s_done = true;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// UI: a user tapping through the apps

static int s_ui_fail, s_ui_steps;

static void expect_home(bool want, const char *what)
{
    s_ui_steps++;
    if (ui_is_home() != want) {
        s_ui_fail++;
        AT("UI FAIL: %s (home=%d, expected %d)", what, ui_is_home(), want);
    }
}

static void go_home(void)
{
    if (!ui_is_home()) {
        dbg_touch_tap(STATUS_BAR);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}

static void open_card(int x, int y, int scrolls, int stay_ms, const char *name)
{
    char what[48];
    go_home();
    for (int i = 0; i < scrolls; i++) {
        dbg_touch_swipe(748, 480, 748, 150, 400);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    dbg_touch_tap(x, y);
    vTaskDelay(pdMS_TO_TICKS(1500));
    snprintf(what, sizeof(what), "%s opens", name);
    expect_home(false, what);
    vTaskDelay(pdMS_TO_TICKS(stay_ms));
    dbg_touch_tap(STATUS_BAR);
    vTaskDelay(pdMS_TO_TICKS(1200));
    snprintf(what, sizeof(what), "status bar -> home from %s", name);
    expect_home(true, what);
}

static void ui_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));
    int cycle = 0;
    while (!s_done) {
        open_card(CARD_FUEL, 0, 4000, "fuel");      // first thing: fuel while downloads run
        open_card(CARD_WEATHER, 0, 3000, "weather");
        open_card(CARD_GPS, 0, 3000, "gps");
        open_card(CARD_TRAFFIC, 0, 3000, "traffic");
        s_radar_busy = true;
        open_card(CARD_RADAR, 0, 9000, "radar");
        vTaskDelay(pdMS_TO_TICKS(3000));                 // radar buffers freed after its delete
        s_radar_busy = false;
        open_card(CARD_TRIP, 0, 3000, "trip");
        open_card(CARD_EXPLORE, 1, 6000, "explore");
        open_card(CARD_CAMERAS, 1, 3000, "cameras");
        open_card(CARD_PARKING, 1, 2000, "parked");
        open_card(CARD_SETTING, 2, 2000, "settings");
        // A hurried double tap, then idle on home for a while (like driving with the home screen up).
        go_home();
        dbg_touch_tap(CARD_FUEL);
        dbg_touch_tap(CARD_FUEL);
        vTaskDelay(pdMS_TO_TICKS(2000));
        go_home();
        vTaskDelay(pdMS_TO_TICKS(20000));
        AT("ui cycle %d done (%d steps, %d failures)", ++cycle, s_ui_steps, s_ui_fail);
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Monitor

static int64_t s_refr_start;
static volatile uint32_t s_refr_max_ms, s_refr_window_ms;

static void on_refr(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_start = esp_timer_get_time();
    } else if (s_refr_start) {
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - s_refr_start) / 1000);
        s_refr_window_ms = ms > s_refr_window_ms ? ms : s_refr_window_ms;
        s_refr_max_ms = ms > s_refr_max_ms ? ms : s_refr_max_ms;
    }
}

static const char *const TASKS[] = { "net_worker", "lvgl", "touch", "esp_timer", "sys_evt", "tiT", "wifi", "httpd",
                                     "soak_ui", "soak_drive" };
#define NTASKS (sizeof(TASKS) / sizeof(TASKS[0]))
static uint32_t s_hwm_min[NTASKS];

// Frame-buffer check. The frame on screen ("front" buffer) is compared with a fresh snapshot of the
// screen, rows 64..469 (below the status bar, above banners/toasts). A row counts as bad when >= 16
// pixels differ clearly; a pending redraw explains that for a moment, so only rows that stay bad,
// with identical screen and snapshot content, over 3 samples 1.5 s apart count as stuck garbage.
#define FB_Y0   64
#define FB_Y1   470
#define FB_ROWS (FB_Y1 - FB_Y0)
typedef struct {
    uint32_t snap_crc, front_crc;
    bool bad;
} row_t;
static row_t s_rows[3][FB_ROWS];
static int s_fb_checks, s_fb_skipped, s_fb_stuck;

static uint32_t crc(const uint16_t *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static bool differs(uint16_t a, uint16_t b)
{
    const int dr = abs((a >> 11) - (b >> 11)), dg = abs(((a >> 5) & 63) - ((b >> 5) & 63)), db = abs((a & 31) - (b & 31));
    return dr * 2 + dg + db * 2 > 12;
}

static bool fb_sample(row_t *rows)
{
    if (!ui_lock()) {
        return false;
    }
    bool ok = false;
    void *fb0 = NULL, *fb1 = NULL;
    lv_display_t *disp = lv_display_get_default();
    lv_draw_buf_t *act = lv_display_get_buf_active(disp);
    lv_draw_buf_t *snap = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) == ESP_OK && act &&
        (snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565)) != NULL) {
        const uint16_t *front = (const uint16_t *)(act->data == fb0 ? fb1 : fb0);
        const uint32_t sstride = snap->header.stride / 2;
        for (int r = 0; r < FB_ROWS; r++) {
            if (r % 32 == 31) {
                vTaskDelay(1);  // like the product's big loops: let the panel's scan-out read PSRAM
            }
            const uint16_t *f = front + (FB_Y0 + r) * BOARD_LCD_H_RES;
            const uint16_t *s = (const uint16_t *)snap->data + (FB_Y0 + r) * sstride;
            int n = 0;
            for (int x = 0; x < BOARD_LCD_H_RES && n < 16; x++) {
                n += differs(f[x], s[x]);
            }
            rows[r] = (row_t){ crc(s, BOARD_LCD_H_RES), crc(f, BOARD_LCD_H_RES), n >= 16 };
        }
        lv_draw_buf_destroy(snap);
        ok = true;
    }
    ui_unlock();
    return ok;
}

static void fb_check(void)
{
    if (s_radar_busy) {
        s_fb_skipped++;
        return;
    }
    for (int k = 0; k < 3; k++) {
        if (k) {
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        if (s_radar_busy || !fb_sample(s_rows[k])) {
            s_fb_skipped++;
            return;
        }
    }
    s_fb_checks++;
    int stuck = 0, first = -1;
    for (int r = 0; r < FB_ROWS; r++) {
        const row_t *a = &s_rows[0][r], *b = &s_rows[1][r], *c = &s_rows[2][r];
        if (a->bad && b->bad && c->bad && a->snap_crc == b->snap_crc && b->snap_crc == c->snap_crc &&
            a->front_crc == b->front_crc && b->front_crc == c->front_crc) {
            stuck++;
            first = first < 0 ? FB_Y0 + r : first;
        }
    }
    if (stuck) {
        s_fb_stuck++;
        AT("FB: %d rows on screen differ from the render and stay (first y=%d, home=%d)", stuck, first, ui_is_home());
    }
}

static uint32_t s_int_min = UINT32_MAX, s_psram_largest_min = UINT32_MAX;

static void monitor_task(void *arg)
{
    for (size_t i = 0; i < NTASKS; i++) {
        s_hwm_min[i] = UINT32_MAX;
    }
    int64_t last_report = 0;
    heap_caps_monitor_local_minimum_free_size_start();  // per-window internal minimum (see below)
    int64_t last_fb = 0;
    while (!s_done) {
        // The check itself streams ~8 MB through PSRAM (3 snapshots + compares): once a minute, so
        // what's seen on screen during the soak is the product, not the checker.
        if (esp_timer_get_time() - last_fb >= 60 * 1000000LL) {
            fb_check();
            last_fb = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
        const int64_t now = esp_timer_get_time();
        if (now - last_report < 30 * 1000000LL) {
            continue;
        }
        last_report = now;
        const uint32_t ifree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // Minimum of this window only, to match a dip with what the log shows around it.
        const uint32_t imin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        heap_caps_monitor_local_minimum_free_size_stop();
        heap_caps_monitor_local_minimum_free_size_start();
        const uint32_t ilarge = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const uint32_t pfree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t plarge = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        s_int_min = imin < s_int_min ? imin : s_int_min;
        if (!s_radar_busy) {
            s_psram_largest_min = plarge < s_psram_largest_min ? plarge : s_psram_largest_min;
        }
        char hwm[200] = "";
        size_t len = 0;
        for (size_t i = 0; i < NTASKS; i++) {
            TaskHandle_t h = xTaskGetHandle(TASKS[i]);
            if (!h) {
                continue;
            }
            const uint32_t w = uxTaskGetStackHighWaterMark(h);
            s_hwm_min[i] = w < s_hwm_min[i] ? w : s_hwm_min[i];
            len += snprintf(hwm + len, sizeof(hwm) - len, " %s=%lu", TASKS[i], (unsigned long)w);
        }
        AT("mon: internal %lu (window min %lu, largest %lu), psram %lu (largest %lu), refresh max %lu ms, errors %d, fb %d/%d skip %d",
           (unsigned long)ifree, (unsigned long)imin, (unsigned long)ilarge, (unsigned long)pfree, (unsigned long)plarge,
           (unsigned long)s_refr_window_ms, s_errors, s_fb_checks, s_fb_stuck, s_fb_skipped);
        AT("mon: stack free:%s", hwm);
        s_refr_window_ms = 0;
    }

    // Final verdict
    char what[160];
    snprintf(what, sizeof(what), "UI: %d steps, %d failures", s_ui_steps, s_ui_fail);
    check(s_ui_fail == 0 && s_ui_steps > 20, what);
    snprintf(what, sizeof(what), "Radar always got its map buffers (%d failures)", s_radar_nomem);
    check(s_radar_nomem == 0, what);
    snprintf(what, sizeof(what), "frame buffer: %d checks, %d with stuck rows (%d skipped)", s_fb_checks, s_fb_stuck,
             s_fb_skipped);
    check(s_fb_stuck == 0 && s_fb_checks >= 15, what);
    snprintf(what, sizeof(what), "internal RAM min ever %lu B (>= 16 KB)", (unsigned long)s_int_min);
    check(s_int_min >= 16 * 1024, what);
    snprintf(what, sizeof(what), "PSRAM largest free block (Radar closed) min %lu B (>= 2.4 MB)",
             (unsigned long)s_psram_largest_min);
    check(s_psram_largest_min >= 2400 * 1024, what);
    snprintf(what, sizeof(what), "slowest LVGL refresh %lu ms (< 2000)", (unsigned long)s_refr_max_ms);
    check(s_refr_max_ms < 2000, what);
    for (size_t i = 0; i < NTASKS; i++) {
        if (s_hwm_min[i] != UINT32_MAX) {
            snprintf(what, sizeof(what), "stack headroom %s %lu B (>= 512)", TASKS[i], (unsigned long)s_hwm_min[i]);
            check(s_hwm_min[i] >= 512, what);
        }
    }
    AT("network error lines (DNS/connect/TLS, tolerated): %d", s_net_errors);
    snprintf(what, sizeof(what), "no other ESP_LOGE lines (%d)", s_errors);
    check(s_errors == 0, what);
    for (int i = 0; i < s_errors && i < 4; i++) {
        AT("  error: %s", s_first_errors[i]);
    }
    settings_set_night_mode(s_prev_night);  // restore the user's setting
    if (s_fail) {
        AT("RESULT: FAIL (%d)", s_fail);
    } else {
        AT("RESULT: PASS");
    }
    vTaskDelete(NULL);
}

void soak_start(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    s_prev_vprintf = esp_log_set_vprintf(soak_vprintf);
    if (ui_lock()) {
        s_prev_night = settings_get_night_mode();
        settings_set_night_mode(NIGHT_MODE_OFF);  // the dark overlay isn't in snapshots (restored at the end)
        ui_night_update();
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, NULL);
        ui_unlock();
    }
    AT("start: simulated drive + UI user + monitor, ~35 min");
    xTaskCreatePinnedToCoreWithCaps(drive_task, "soak_drive", 6144, NULL, 2, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(ui_task, "soak_ui", 12288, NULL, 2, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(monitor_task, "soak_mon", 8192, NULL, 1, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
}
