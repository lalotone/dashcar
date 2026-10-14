// Speed camera alerts: a banner on the top layer (any screen) when a fixed camera is ahead, and
// the running average speed inside an average-speed section. Checked once a second on the LVGL
// task from the GPS fix; no network. The DGT data has no headings, so "ahead" means the camera's
// bearing is close to the car's course (a camera for the other carriageway can still alert).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "services/cameras.h"
#include "services/fuel.h"   // fuel_distance_km
#include "services/gps.h"
#include "settings.h"
#include "ui.h"

#define MIN_KMH         20.0f
#define LOOKAHEAD_S     30         // alert this many seconds before a fixed camera...
#define ALERT_MIN_KM    0.4f       // ...but at least this far
#define ALERT_MAX_KM    1.5f
#define AHEAD_DEG       30.0f      // camera bearing vs. course
#define NEAR_KM         0.15f      // closer than this, allow a wider angle (curves)
#define SECTION_ENTER_KM 0.35f
#define SECTION_EXIT_KM 0.2f
#define SECTION_MAX_S   (40 * 60)
#define SECTION_AWAY_S  30         // moving away from the exit this long = left the section
#define RESULT_S        8          // show the final average this long
#define C_FIXED         lv_color_hex(0xB91C1C)
#define C_SECTION       lv_color_hex(0xB45309)

static const char *TAG = "cam_alert";

static lv_obj_t *s_banner, *s_title, *s_sub;
static float s_last_lat, s_last_lon;   // last fixed camera alerted (logged once per camera)
static bool s_enabled;
static lv_color_t s_shown_color;

static struct {
    bool active;
    camera_t cam;              // copy: the data array can be replaced while we're inside
    float exit_lat, exit_lon;
    float km;                  // travelled inside the section
    float best_exit_km;        // closest we've been to the exit
    int away_s;
    int64_t start_us, last_us;
    int result_s;              // > 0: showing the final average
    float result_kmh;
} s_sec;

static float bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    const float r = (float)M_PI / 180;
    const float y = sinf((lon2 - lon1) * r) * cosf(lat2 * r);
    const float x = cosf(lat1 * r) * sinf(lat2 * r) - sinf(lat1 * r) * cosf(lat2 * r) * cosf((lon2 - lon1) * r);
    return fmodf(atan2f(y, x) / r + 360, 360);
}

static float angle_diff(float a, float b)
{
    const float d = fabsf(fmodf(a - b + 540, 360) - 180);
    return d;
}

static bool ahead(const gps_fix_t *f, float lat, float lon, float km)
{
    const float diff = angle_diff(bearing_deg((float)f->lat, (float)f->lon, lat, lon), f->course);
    return diff < AHEAD_DEG || (km < NEAR_KM && diff < 2 * AHEAD_DEG);
}

static void fmt_dist(char *buf, size_t len, float km)
{
    if (settings_get_imperial()) {
        snprintf(buf, len, "%.1f mi", km / 1.609344f);
    } else if (km < 1) {
        snprintf(buf, len, "%d m", (int)(km * 1000 / 10) * 10);
    } else {
        snprintf(buf, len, "%.1f km", km);
    }
}

static void fmt_speed(char *buf, size_t len, float kmh)
{
    const bool imp = settings_get_imperial();
    snprintf(buf, len, "%ld %s", lroundf(imp ? kmh / 1.609344f : kmh), imp ? "mph" : "km/h");
}

static void where(char *buf, size_t len, const camera_t *c)
{
    char km[16] = "";
    if (c->km >= 0) {
        snprintf(km, sizeof(km), " km %.0f", c->km);
    }
    snprintf(buf, len, "%s%s%s%s", c->road, km, c->toward[0] ? "  ·  towards " : "", c->toward);
}

static void show(lv_color_t color, const char *title, const char *sub)
{
    if (!lv_color_eq(color, s_shown_color)) {
        lv_obj_set_style_bg_color(s_banner, color, 0);
        s_shown_color = color;
    }
    ui_label_set_text_if_changed(s_title, title);
    ui_label_set_text_if_changed(s_sub, sub);
    ui_set_hidden_if_changed(s_banner, false);
}

// Average-speed section state machine. Returns true if it has something to show.
static bool section_update(const gps_fix_t *f, bool live, const camera_t *cams, int n, char *title, char *sub,
                           size_t len)
{
    const int64_t now = esp_timer_get_time();
    if (s_sec.result_s > 0) {
        s_sec.result_s--;
        char v[24];
        fmt_speed(v, sizeof(v), s_sec.result_kmh);
        snprintf(title, len, "Section ended  ·  your average %s", v);
        snprintf(sub, len, "%s", s_sec.cam.road);
        return true;
    }
    if (!s_sec.active) {
        if (!live || f->speed_kmh < MIN_KMH) {
            return false;
        }
        for (int i = 0; i < n; i++) {
            const camera_t *c = &cams[i];
            if (c->kind != CAM_SECTION ||
                (fabsf(c->lat - (float)f->lat) > 0.01f && fabsf(c->lat2 - (float)f->lat) > 0.01f)) {
                continue;
            }
            for (int end = 0; end < 2; end++) {
                const float la = end ? c->lat2 : c->lat, lo = end ? c->lon2 : c->lon;
                const float d = fuel_distance_km((float)f->lat, (float)f->lon, la, lo);
                if (d < SECTION_ENTER_KM && ahead(f, la, lo, d)) {
                    memset(&s_sec, 0, sizeof(s_sec));
                    s_sec.active = true;
                    s_sec.cam = *c;
                    s_sec.exit_lat = end ? c->lat : c->lat2;
                    s_sec.exit_lon = end ? c->lon : c->lon2;
                    s_sec.best_exit_km = 1e9f;
                    s_sec.start_us = s_sec.last_us = now;
                    ESP_LOGI(TAG, "average-speed section entered: %s", c->road);
                    break;
                }
            }
            if (s_sec.active) {
                break;
            }
        }
        if (!s_sec.active) {
            return false;
        }
    }
    // Inside: integrate the distance from the GPS speed (works through curves, unlike a straight line).
    const float dt = (now - s_sec.last_us) / 1e6f;
    s_sec.last_us = now;
    if (live && dt > 0 && dt < 5) {
        s_sec.km += f->speed_kmh * dt / 3600;
    }
    const float elapsed_s = (now - s_sec.start_us) / 1e6f;
    const float avg = elapsed_s > 5 ? s_sec.km / elapsed_s * 3600 : f->speed_kmh;
    const float to_exit = fuel_distance_km((float)f->lat, (float)f->lon, s_sec.exit_lat, s_sec.exit_lon);
    if (to_exit < s_sec.best_exit_km) {
        s_sec.best_exit_km = to_exit;
        s_sec.away_s = 0;
    } else if (to_exit > s_sec.best_exit_km + 0.3f) {
        s_sec.away_s++;
    }
    if (to_exit < SECTION_EXIT_KM || s_sec.away_s > SECTION_AWAY_S || elapsed_s > SECTION_MAX_S) {
        s_sec.active = false;
        s_sec.result_s = to_exit < SECTION_EXIT_KM ? RESULT_S : 0;
        s_sec.result_kmh = avg;
        ESP_LOGI(TAG, "average-speed section %s: %.1f km, average %.0f km/h", to_exit < SECTION_EXIT_KM ? "ended" : "left",
                 s_sec.km, avg);
        return false;
    }
    char v[24], d[24];
    fmt_speed(v, sizeof(v), avg);
    fmt_dist(d, sizeof(d), to_exit);
    snprintf(title, len, "Average speed section  ·  your average %s", v);
    snprintf(sub, len, "%s  ·  about %s to the end", s_sec.cam.road, d);
    return true;
}

static void check(lv_timer_t *t)
{
    (void)t;
    gps_fix_t f;
    gps_get(&f);
    int sats;
    const bool live = gps_state(&sats) == GPS_FIX;
    const camera_t *cams;
    const int n = cameras_list(&cams);
    if (!s_enabled || n == 0) {
        s_sec.active = false;
        ui_set_hidden_if_changed(s_banner, true);
        return;
    }

    // Fixed cameras first: the nearest one ahead within ~30 s of driving.
    char title[96], sub[96];
    if (live && f.speed_kmh >= MIN_KMH) {
        const float range = fminf(ALERT_MAX_KM, fmaxf(ALERT_MIN_KM, f.speed_kmh * LOOKAHEAD_S / 3600));
        const camera_t *best = NULL;
        float best_d = range;
        for (int i = 0; i < n; i++) {
            const camera_t *c = &cams[i];
            if (c->kind != CAM_FIXED || fabsf(c->lat - (float)f.lat) > 0.02f || fabsf(c->lon - (float)f.lon) > 0.03f) {
                continue;
            }
            const float d = fuel_distance_km((float)f.lat, (float)f.lon, c->lat, c->lon);
            if (d < best_d && ahead(&f, c->lat, c->lon, d)) {
                best = c;
                best_d = d;
            }
        }
        if (best) {
            if (best->lat != s_last_lat || best->lon != s_last_lon) {
                s_last_lat = best->lat;
                s_last_lon = best->lon;
                ESP_LOGI(TAG, "fixed camera ahead: %s km %.0f, %.0f m", best->road, best->km, best_d * 1000);
            }
            section_update(&f, live, cams, n, title, sub, sizeof(title));  // keep integrating
            char d[24];
            fmt_dist(d, sizeof(d), best_d);
            snprintf(title, sizeof(title), LV_SYMBOL_WARNING "  Speed camera  ·  %s", d);
            where(sub, sizeof(sub), best);
            show(C_FIXED, title, sub);
            return;
        }
    }
    if (section_update(&f, live, cams, n, title, sub, sizeof(title))) {
        show(C_SECTION, title, sub);
        return;
    }
    ui_set_hidden_if_changed(s_banner, true);
}

void ui_cam_alert_set_enabled(bool on)
{
    s_enabled = on;
    settings_set_cam_alerts(on);
}

bool ui_cam_alert_enabled(void)
{
    return s_enabled;
}

void ui_cam_alert_init(void)
{
    s_enabled = settings_get_cam_alerts();
    s_banner = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, 760, LV_SIZE_CONTENT);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(s_banner, 22, 0);
    lv_obj_set_style_bg_color(s_banner, C_FIXED, 0);
    s_shown_color = C_FIXED;
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_banner, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_banner, 2, 0);
    lv_obj_set_style_border_opa(s_banner, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(s_banner, 24, 0);
    lv_obj_set_style_pad_ver(s_banner, 12, 0);
    lv_obj_set_style_pad_row(s_banner, 2, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(s_banner, false);   // taps go through to the screen
    lv_obj_set_scrollable(s_banner, false);
    s_title = ui_label_create(s_banner, ui_font_lg, lv_color_white(), "");
    s_sub = ui_label_create(s_banner, ui_font_md, lv_color_white(), "");
    lv_obj_set_width(s_sub, lv_pct(100));
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_hidden(s_banner, true);
    lv_timer_create(check, 1000, NULL);
}
