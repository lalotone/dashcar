// Explore: overlanding places around the car (water, campsites, huts/shelters, viewpoints, picnic sites,
// toilets) from OpenStreetMap, like the Fuel screen: a list by distance, tap for the coordinates and a
// Google Maps QR. Runs only while open: the ~21 KB list is allocated on open and freed on close, one
// network-worker job loads the nearest saved area (flash) and downloads only if it's missing, old or
// far away. Nothing runs in the background.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net/wifi_mgr.h"
#include "services/fuel.h"        // fuel_distance_km
#include "services/location.h"
#include "services/net_worker.h"
#include "services/poi.h"
#include "ui.h"

#define TOP_Y       (UI_HEADER_H + 4)
#define CHIPS_H     52
#define BODY_Y      (TOP_Y + CHIPS_H + 12)
#define BODY_H      (600 - BODY_Y - UI_PAD)
#define LEFT_W      300
#define RIGHT_W     (1024 - LEFT_W - 3 * UI_PAD)
#define MAX_ROWS    20                    // each row ~40 ms to build + draw
#define FRESH_S     (7 * 24 * 3600)       // a saved area younger than this...
#define NEAR_KM     5.0f                  // ...and centred this close is used as is
#define SAVED_KM    20.0f                 // offline: any saved area this close

static const uint32_t KIND_COLOR[POI_KINDS] = { 0x38BDF8, 0x34D399, 0xF59E0B, 0xA78BFA, 0xF472B6, 0x94A3B8 };
static const char *const CHIP_LABEL[POI_KINDS + 1] = { "All", "Water", "Camp", "Huts", "Views", "Picnic", "WC" };

typedef struct {
    const poi_t *p;
    float km;
} near_t;

typedef struct {
    poi_area_t area;              // items: POI_MAX, PSRAM
    near_t near[POI_MAX];         // render's sort buffer (here, not on whichever task's stack renders)
    char status[96];
    bool loading;
} explore_t;                      // ~3.4 KB + 21 KB items, freed on close

static lv_obj_t *s_scr;
static explore_t *s_ex;           // this screen instance's data (LVGL task / under ui_lock)
static uint32_t s_gen;            // bumped on open/close: a job for an old instance discards its result
static lv_obj_t *s_status, *s_counts, *s_list, *s_list_title;
static lv_obj_t *s_chips[POI_KINDS + 1];
static int s_filter = -1;         // -1 = all (session only)

// ---------------------------------------------------------------------------
// Background job (network worker): saved area first, then a download if needed.

typedef struct {
    uint32_t gen;
    float lat, lon;
    bool force;
} job_t;

static void deliver(uint32_t gen, const poi_area_t *a, const char *status, bool loading);

static void fetch_job(void *arg)
{
    job_t *j = arg;
    poi_t *items = heap_caps_malloc(POI_MAX * sizeof(poi_t), MALLOC_CAP_SPIRAM);
    if (!items) {
        deliver(j->gen, NULL, "Not enough memory", false);
        heap_caps_free(j);
        return;
    }
    poi_area_t a = { .items = items };
    char status[96];
    const time_t now = time(NULL);
    bool have = poi_cache_load_nearest(j->lat, j->lon, SAVED_KM, &a);
    const bool fresh = have && now > 1700000000 && now - a.fetched < FRESH_S &&
                       fuel_distance_km(j->lat, j->lon, a.lat, a.lon) < NEAR_KM;
    const bool download = j->force || !fresh;
    if (have) {
        deliver(j->gen, &a, download ? "Saved area, updating..." : "", download);
    }
    if (download && wifi_mgr_state() == WIFI_MGR_CONNECTED && s_gen == j->gen) {
        poi_area_t fresh_area = { .items = items };
        if (poi_fetch(j->lat, j->lon, &fresh_area) == ESP_OK) {
            deliver(j->gen, &fresh_area, "", false);
        } else {
            have = have && poi_cache_load_nearest(j->lat, j->lon, SAVED_KM, &a);  // items were overwritten
            snprintf(status, sizeof(status), have ? "Map server busy: showing the saved area" :
                                                    "Map server busy. Try again in a minute.");
            deliver(j->gen, have ? &a : NULL, status, false);
        }
    } else if (download) {
        deliver(j->gen, have ? &a : NULL, have ? "Offline: showing the saved area" : "No connection and no saved area here",
                false);
    }
    heap_caps_free(items);
    heap_caps_free(j);
}

static void start_job(bool force)
{
    location_t loc;
    if (!s_ex || s_ex->loading) {
        return;
    }
    if (!location_get(&loc)) {
        strlcpy(s_ex->status, "Waiting for your location", sizeof(s_ex->status));
        return;
    }
    job_t *j = heap_caps_malloc(sizeof(*j), MALLOC_CAP_SPIRAM);
    if (!j) {
        return;
    }
    *j = (job_t){ .gen = s_gen, .lat = loc.lat, .lon = loc.lon, .force = force };
    s_ex->loading = true;
    strlcpy(s_ex->status, "Loading places...", sizeof(s_ex->status));
    if (!net_worker_post(fetch_job, j)) {
        heap_caps_free(j);
        s_ex->loading = false;
        strlcpy(s_ex->status, "Busy, please try again", sizeof(s_ex->status));
    }
}

// ---------------------------------------------------------------------------
// Rendering

static const char *compass8(float deg)
{
    static const char *const N[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return N[(int)lroundf(fmodf(deg + 360, 360) / 45) % 8];
}

static float bearing(float lat1, float lon1, float lat2, float lon2)
{
    const float r = (float)M_PI / 180;
    return atan2f((lon2 - lon1) * cosf(lat1 * r), lat2 - lat1) / r;
}

static void fmt_dist(char *buf, size_t len, float km)
{
    if (km < 1) {
        snprintf(buf, len, "%d m", (int)(km * 1000 / 10) * 10);
    } else {
        snprintf(buf, len, km < 10 ? "%.1f km" : "%.0f km", km);
    }
}

static const char *poi_title(const poi_t *p)
{
    return p->name[0] ? p->name : poi_kind_name(p->kind);
}

static void on_popup_close(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
}

static void show_detail(const poi_t *p)
{
    location_t loc;
    const bool have_loc = location_get(&loc);
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_add_event_cb(bg, on_popup_close, LV_EVENT_CLICKED, bg);

    lv_obj_t *card = ui_card_create(bg);
    lv_obj_set_size(card, 820, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_column(card, 28, 0);
    lv_obj_add_event_cb(card, on_popup_close, LV_EVENT_CLICKED, bg);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 10, 0);
    lv_obj_set_clickable(info, false);

    char buf[128], d[16];
    lv_obj_t *t = ui_label_create(info, ui_font_lg, UI_COLOR_TEXT, poi_title(p));
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    ui_label_create(info, ui_font_md, lv_color_hex(KIND_COLOR[p->kind]), poi_kind_name(p->kind));
    snprintf(buf, sizeof(buf), "%.5f, %.5f", p->lat, p->lon);
    ui_label_create(info, ui_font_xl, UI_COLOR_TEXT, buf);
    if (have_loc) {
        fmt_dist(d, sizeof(d), fuel_distance_km(loc.lat, loc.lon, p->lat, p->lon));
        snprintf(buf, sizeof(buf), "%s %s of you", d, compass8(bearing(loc.lat, loc.lon, p->lat, p->lon)));
        ui_label_create(info, ui_font_md, UI_COLOR_MUTED, buf);
    }
    buf[0] = '\0';
    if (p->ele != INT16_MIN) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "Elevation %d m   ", p->ele);
    }
    if (p->flags & POI_FEE) {
        strlcat(buf, "Fee   ", sizeof(buf));
    }
    if (p->flags & POI_NOT_POTABLE) {
        strlcat(buf, "Not drinkable   ", sizeof(buf));
    }
    if (p->flags & POI_OPEN_24H) {
        strlcat(buf, "24/7", sizeof(buf));
    }
    if (buf[0]) {
        ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, buf);
    }
    ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, "Tap anywhere to close");

    lv_obj_t *qr_col = lv_obj_create(card);
    lv_obj_remove_style_all(qr_col);
    lv_obj_set_size(qr_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qr_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qr_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qr_col, 10, 0);
    lv_obj_set_clickable(qr_col, false);
    char url[96];
    snprintf(url, sizeof(url), "https://www.google.com/maps/dir/?api=1&destination=%.5f,%.5f", p->lat, p->lon);
    lv_obj_t *qr = lv_qrcode_create(qr_col);
    lv_qrcode_set_size(qr, 230);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_clickable(qr, false);
    ui_label_create(qr_col, ui_font_md, UI_COLOR_TEXT, LV_SYMBOL_GPS "  Scan to navigate");
    ui_label_create(qr_col, ui_font_sm, UI_COLOR_MUTED, "Opens Google Maps on your phone");
}

static void on_row(lv_event_t *e)
{
    show_detail(lv_event_get_user_data(e));
}

static int cmp_near(const void *a, const void *b)
{
    const float d = ((const near_t *)a)->km - ((const near_t *)b)->km;
    return d < 0 ? -1 : d > 0;
}

static void render(void)
{
    if (!s_scr || !s_ex) {
        return;
    }
    explore_t *ex = s_ex;
    for (int i = 0; i <= POI_KINDS; i++) {
        const bool on = i - 1 == s_filter;
        lv_obj_set_style_bg_color(s_chips[i], on ? UI_COLOR_ACCENT : UI_COLOR_CARD_HI, 0);
    }

    // Counts per kind (left card)
    int counts[POI_KINDS] = { 0 };
    for (int i = 0; i < ex->area.count; i++) {
        counts[ex->area.items[i].kind]++;
    }
    char buf[160];
    size_t len = 0;
    buf[0] = '\0';
    for (int k = 0; k < POI_KINDS; k++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s%-14s %d", k ? "\n" : "", poi_kind_name(k), counts[k]);
    }
    ui_label_set_text_if_changed(s_counts, ex->area.count ? buf : "");

    if (ex->status[0]) {
        strlcpy(buf, ex->status, sizeof(buf));
    } else if (ex->area.count) {
        const long mins = (long)(time(NULL) - ex->area.fetched) / 60;
        if (mins < 60) {
            snprintf(buf, sizeof(buf), "%d places\nwithin %d km\nupdated %ld min ago", ex->area.count, POI_RADIUS_KM,
                     mins < 0 ? 0 : mins);
        } else if (mins < 48 * 60) {
            snprintf(buf, sizeof(buf), "%d places\nwithin %d km\nupdated %ld h ago", ex->area.count, POI_RADIUS_KM, mins / 60);
        } else {
            snprintf(buf, sizeof(buf), "%d places\nwithin %d km\nupdated %ld days ago", ex->area.count, POI_RADIUS_KM,
                     mins / (24 * 60));
        }
    } else {
        strlcpy(buf, "No places found here", sizeof(buf));
    }
    ui_label_set_text_if_changed(s_status, buf);

    // List: the nearest matches from where the car is now.
    lv_obj_clean(s_list);
    location_t loc;
    const bool have_loc = location_get(&loc);
    near_t *near = ex->near;
    int n = 0;
    for (int i = 0; i < ex->area.count; i++) {
        const poi_t *p = &ex->area.items[i];
        if (s_filter >= 0 && p->kind != s_filter) {
            continue;
        }
        near[n++] = (near_t){ p, have_loc ? fuel_distance_km(loc.lat, loc.lon, p->lat, p->lon) : 0 };
    }
    qsort(near, n, sizeof(near[0]), cmp_near);
    if (n > MAX_ROWS) {
        snprintf(buf, sizeof(buf), "%d places  (nearest %d)", n, MAX_ROWS);
    } else {
        snprintf(buf, sizeof(buf), "%d place%s", n, n == 1 ? "" : "s");
    }
    ui_label_set_text_if_changed(s_list_title, ex->loading && n == 0 ? "Loading..." : buf);
    if (n == 0 && ex->loading) {
        lv_obj_t *sp = lv_spinner_create(s_list);
        lv_obj_set_size(sp, 56, 56);
        return;
    }
    for (int i = 0; i < n && i < MAX_ROWS; i++) {
        const poi_t *p = near[i].p;
        lv_obj_t *row = ui_list_row(s_list, on_row, (void *)p);
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 22, 22);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(KIND_COLOR[p->kind]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_clickable(dot, false);

        if (p->name[0]) {
            snprintf(buf, sizeof(buf), "%s  ·  %.5f, %.5f", poi_kind_name(p->kind), p->lat, p->lon);
        } else {
            snprintf(buf, sizeof(buf), "%.5f, %.5f", p->lat, p->lon);  // the title already says the kind
        }
        ui_list_row_column(row, poi_title(p), buf);

        if (have_loc) {
            char d[16];
            fmt_dist(d, sizeof(d), near[i].km);
            lv_obj_t *right = lv_obj_create(row);
            lv_obj_remove_style_all(right);
            lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
            lv_obj_set_clickable(right, false);
            ui_label_create(right, ui_font_lg, UI_COLOR_TEXT, d);
            ui_label_create(right, ui_font_md, UI_COLOR_MUTED, compass8(bearing(loc.lat, loc.lon, p->lat, p->lon)));
        }
    }
}

// Under ui_lock: copy a result into this screen instance if it's still open.
static void deliver(uint32_t gen, const poi_area_t *a, const char *status, bool loading)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr && s_ex && gen == s_gen) {
        if (a) {
            memcpy(s_ex->area.items, a->items, a->count * sizeof(poi_t));
            s_ex->area.count = a->count;
            s_ex->area.lat = a->lat;
            s_ex->area.lon = a->lon;
            s_ex->area.fetched = a->fetched;
        }
        strlcpy(s_ex->status, status, sizeof(s_ex->status));
        s_ex->loading = loading;
        render();
    }
    ui_unlock();
}

// ---------------------------------------------------------------------------
// Events

static void on_chip(lv_event_t *e)
{
    s_filter = (int)(intptr_t)lv_event_get_user_data(e);
    render();
}

static void on_refresh(lv_event_t *e)
{
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        ui_toast("No Wi-Fi connection");
        return;
    }
    start_job(true);
    render();
}

static void on_follow(lv_timer_t *t)
{
    render();  // distances and order from where the car is now (LOCATION_FOLLOW_MS)
}

static void on_back(lv_event_t *e)
{
    ui_home_open();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    s_gen++;     // an in-flight job discards its result
    s_scr = NULL;
    if (s_ex) {
        heap_caps_free(s_ex->area.items);
        heap_caps_free(s_ex);
        s_ex = NULL;
    }
}

void ui_explore_open(void)
{
    explore_t *ex = heap_caps_calloc(1, sizeof(*ex), MALLOC_CAP_SPIRAM);
    poi_t *items = heap_caps_malloc(POI_MAX * sizeof(poi_t), MALLOC_CAP_SPIRAM);
    if (!ex || !items) {
        heap_caps_free(ex);
        heap_caps_free(items);
        ui_toast("Not enough memory");
        return;
    }
    if (s_ex) {  // previous instance not deleted yet (double tap): it keeps nothing we need
        heap_caps_free(s_ex->area.items);
        heap_caps_free(s_ex);
    }
    ex->area.items = items;
    s_ex = ex;
    s_gen++;

    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_t *hdr = ui_header_create(scr, "Explore", on_back);
    ui_header_button(hdr, LV_SYMBOL_REFRESH, NULL, on_refresh, NULL);

    // Category chips
    lv_obj_t *chips = lv_obj_create(scr);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, 1024 - 2 * UI_PAD, CHIPS_H);
    lv_obj_set_pos(chips, UI_PAD, TOP_Y);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 10, 0);
    lv_obj_set_scrollable(chips, false);
    for (int i = 0; i <= POI_KINDS; i++) {
        lv_obj_t *b = ui_button_create(chips, CHIP_LABEL[i], UI_COLOR_CARD_HI, on_chip, (void *)(intptr_t)(i - 1));
        lv_obj_set_height(b, CHIPS_H);
        lv_obj_set_style_pad_hor(b, 18, 0);
        s_chips[i] = b;
    }

    // Left: status + counts per kind
    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, BODY_Y);
    lv_obj_set_scrollable(left, false);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 14, 0);
    s_status = ui_label_create(left, ui_font_md, UI_COLOR_TEXT, "");
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    s_counts = ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_t *spacer = lv_obj_create(left);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    lv_obj_t *src = ui_label_create(left, ui_font_sm, UI_COLOR_MUTED,
                                    "Data: (c) OpenStreetMap contributors. Saved areas work offline.");
    lv_obj_set_width(src, lv_pct(100));
    lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);

    // Right: nearest places
    lv_obj_t *right = ui_card_create(scr);
    lv_obj_set_size(right, RIGHT_W, BODY_H);
    lv_obj_set_pos(right, LEFT_W + 2 * UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_set_scrollable(right, false);
    s_list_title = ui_label_create(right, ui_font_md, UI_COLOR_TEXT, "");
    s_list = ui_list_create(right);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    start_job(false);
    render();
    ui_screen_own_timer(scr, lv_timer_create(on_follow, LOCATION_FOLLOW_MS, NULL));
    ui_screen_load(scr);
}
