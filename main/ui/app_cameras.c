// Speed cameras: alerts on/off, data status, and the nearest cameras / average-speed sections.
#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "services/cameras.h"
#include "services/fuel.h"   // fuel_distance_km
#include "services/location.h"
#include "services/weather.h"
#include "settings.h"
#include "ui.h"

#define BODY_Y   (UI_HEADER_H + 8)
#define BODY_H   (600 - BODY_Y - UI_PAD)
#define LEFT_W   360
#define RIGHT_X  (UI_PAD + LEFT_W + UI_PAD)
#define RIGHT_W  (1024 - RIGHT_X - UI_PAD)
#define MAX_ROWS 20
#define MAX_KM   100

static lv_obj_t *s_scr;
static lv_obj_t *s_toggle;
static lv_obj_t *s_status;
static lv_obj_t *s_list;

typedef struct {
    const camera_t *cam;
    float km;
} near_t;

static void on_back(lv_event_t *e)
{
    ui_home_open();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    s_scr = NULL;
}

static int cmp_km(const void *a, const void *b)
{
    const float d = ((const near_t *)a)->km - ((const near_t *)b)->km;
    return d < 0 ? -1 : d > 0;
}

static void fill_toggle(void)
{
    const bool on = ui_cam_alert_enabled();
    lv_obj_t *l = lv_obj_get_child(s_toggle, 0);
    lv_label_set_text(l, on ? LV_SYMBOL_BELL "  Alerts on" : LV_SYMBOL_MUTE "  Alerts off");
    lv_obj_set_style_bg_color(s_toggle, on ? lv_color_hex(0x166534) : UI_COLOR_CARD_HI, 0);
}

static void render(void)
{
    const camera_t *cams;
    const int n = cameras_list(&cams);
    char buf[160];
    const time_t upd = cameras_updated();
    if (n == 0) {
        snprintf(buf, sizeof(buf), cameras_loading() ? "Downloading the DGT camera list..." :
                                   "No camera data yet. It downloads automatically once Wi-Fi is connected.");
    } else {
        int sections = 0;
        for (int i = 0; i < n; i++) {
            sections += cams[i].kind == CAM_SECTION;
        }
        char when[24];
        ui_fmt_local(when, sizeof(when), "%d %b %Y", upd, weather_utc_offset());
        snprintf(buf, sizeof(buf), "%d fixed cameras and %d average-speed sections (DGT, downloaded %s).",
                 n - sections, sections, when);
    }
    ui_label_set_text_if_changed(s_status, buf);

    lv_obj_clean(s_list);
    location_t loc;
    if (n == 0 || !location_get(&loc)) {
        ui_list_add_text(s_list, n ? "Waiting for your location" : "");
        return;
    }
    EXT_RAM_BSS_ATTR static near_t near[1500];
    int m = 0;
    for (int i = 0; i < n && m < 1500; i++) {
        const float d = fuel_distance_km(loc.lat, loc.lon, cams[i].lat, cams[i].lon);
        if (d <= MAX_KM) {
            near[m++] = (near_t){ &cams[i], d };
        }
    }
    qsort(near, m, sizeof(near[0]), cmp_km);
    if (m == 0) {
        ui_list_add_text(s_list, "No cameras within 100 km");
        return;
    }
    const bool imp = settings_get_imperial();
    for (int i = 0; i < m && i < MAX_ROWS; i++) {
        const camera_t *c = near[i].cam;
        char km[16] = "", d[16];
        if (c->km >= 0) {
            snprintf(km, sizeof(km), " km %.0f", c->km);
        }
        if (imp) {
            snprintf(d, sizeof(d), "%.1f mi", near[i].km / 1.609344f);
        } else {
            snprintf(d, sizeof(d), near[i].km < 10 ? "%.1f km" : "%.0f km", near[i].km);
        }
        char title[48];
        snprintf(title, sizeof(title), "%s%s", c->road, km);
        if (c->kind == CAM_SECTION) {
            const float len = fuel_distance_km(c->lat, c->lon, c->lat2, c->lon2);
            snprintf(buf, sizeof(buf), "Average-speed section, ~%.0f %s", imp ? len / 1.609344f : len, imp ? "mi" : "km");
        } else {
            snprintf(buf, sizeof(buf), "Fixed camera%s%s", c->toward[0] ? "  ·  towards " : "", c->toward);
        }
        lv_obj_t *row = ui_list_row(s_list, NULL, NULL);
        ui_list_row_column(row, title, buf);
        ui_label_create(row, ui_font_lg, UI_COLOR_TEXT, d);
    }
}

static void on_toggle(lv_event_t *e)
{
    ui_cam_alert_set_enabled(!ui_cam_alert_enabled());
    fill_toggle();
}

static void on_follow(lv_timer_t *t)
{
    if (s_scr) {
        render();  // re-sort by distance on the GPS cadence (and pick up a finished download)
    }
}

static lv_obj_t *muted(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_MUTED, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

void ui_cameras_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    ui_header_create(scr, "Speed cameras", on_back);

    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, BODY_Y);
    lv_obj_set_scrollable(left, false);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 14, 0);
    ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "ALERTS");
    s_toggle = ui_button_create(left, "", UI_COLOR_CARD_HI, on_toggle, NULL);
    lv_obj_set_size(s_toggle, lv_pct(100), 64);
    lv_obj_set_style_text_font(lv_obj_get_child(s_toggle, 0), ui_font_lg, 0);
    fill_toggle();
    muted(left, "A red banner shows ~30 s before a fixed camera ahead; inside an average-speed section an "
                "amber one shows your running average. Works offline from the saved list.");
    s_status = muted(left, "");
    lv_obj_t *spacer = lv_obj_create(left);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    muted(left, "Not covered: Catalonia and the Basque Country (own networks), mobile radars. The data has no "
                "speed limits.");

    lv_obj_t *right = ui_card_create(scr);
    lv_obj_set_size(right, RIGHT_W, BODY_H);
    lv_obj_set_pos(right, RIGHT_X, BODY_Y);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    ui_label_create(right, ui_font_md, UI_COLOR_TEXT, "Nearest cameras");
    s_list = ui_list_create(right);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    render();
    ui_screen_own_timer(scr, lv_timer_create(on_follow, LOCATION_FOLLOW_MS, NULL));
    ui_screen_load(scr);
}
