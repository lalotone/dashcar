// Fuel app: nearby Spanish fuel stations and prices (Ministerio "Precios Carburantes").
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi_mgr.h"
#include "services/fuel.h"
#include "services/net_worker.h"
#include "services/location.h"
#include "settings.h"
#include "ui.h"

#define CACHE_MAX_AGE_S  (30 * 60)   // the Ministerio updates prices every 30 min
#define MAX_ROWS         20     // list rows actually created (each row costs ~50 ms to build + draw)
#define MAX_VISIBLE      400    // stations considered (sorted) within the radius
#define TOP_Y            (UI_HEADER_H + 4)
#define CHIPS_H          52
#define BODY_Y           (TOP_Y + CHIPS_H + 12)
#define BODY_H           (600 - BODY_Y - UI_PAD)
#define LEFT_W           340
#define RIGHT_W          (1024 - LEFT_W - 3 * UI_PAD)

static const int RADII[] = { 5, 10, 25 };

static lv_obj_t *s_scr;
static lv_obj_t *s_summary;
static lv_obj_t *s_list;
static lv_obj_t *s_list_title;
static lv_obj_t *s_sort_btns[2];
static lv_obj_t *s_product_btns[8];
static lv_obj_t *s_radius_btns[3];
static bool s_sort_by_price = true;
static uint32_t s_gen;
static bool s_loading;
static char s_error[64];
static char s_place[96];

// Last result, kept across screen opens (also feeds the home tile). LVGL lock protects it.
static fuel_result_t s_result;

typedef struct {
    uint32_t gen;
    int product;
    float lat, lon;
} fetch_job_t;

static void render(void);

// ---------------------------------------------------------------------------
// Helpers

static void fmt_price(char *buf, size_t len, float p)
{
    snprintf(buf, len, "%.3f", p);
    char *dot = strchr(buf, '.');
    if (dot) {
        *dot = ',';  // Spanish style, as on the station signs
    }
}

static void fmt_dist(char *buf, size_t len, float km)
{
    if (km < 1.0f) {
        snprintf(buf, len, "%d m", (int)(km * 1000 / 10) * 10);
    } else {
        snprintf(buf, len, "%.1f km", km);
    }
}

static lv_color_t price_color(float p, float lo, float hi)
{
    if (hi - lo < 0.005f) {
        return UI_COLOR_TEXT;
    }
    float t = (p - lo) / (hi - lo);
    return t < 0.34f ? UI_COLOR_OK : t < 0.67f ? UI_COLOR_WARN : UI_COLOR_ERR;
}

static lv_obj_t *plain(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 44);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = ui_label_create(b, ui_font_sm, UI_COLOR_TEXT, text);
    lv_obj_center(l);
    return b;
}

static void chip_set(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_HI, 0);
}

// Stations within the selected radius, in display order. Returns count.
static int visible_stations(const fuel_station_t **out, int max)
{
    const int radius = settings_get_fuel_radius();
    int n = 0;
    for (int i = 0; i < s_result.count && n < max; i++) {
        if (s_result.stations[i].dist_km <= radius) {
            out[n++] = &s_result.stations[i];
        }
    }
    // Already sorted by distance; re-sort by price if asked (stable on distance).
    if (s_sort_by_price) {
        for (int i = 1; i < n; i++) {
            const fuel_station_t *k = out[i];
            int j = i - 1;
            while (j >= 0 && out[j]->price > k->price) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = k;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Fetch

static void fetch_job(void *arg)
{
    fetch_job_t *job = arg;
    fuel_result_t r;
    esp_err_t err = fuel_fetch(job->product, job->lat, job->lon, &r);

    if (ui_lock()) {
        if (job->gen == s_gen) {
            s_loading = false;
            if (err == ESP_OK) {
                fuel_result_free(&s_result);
                s_result = r;
                s_error[0] = '\0';
            } else {
                fuel_result_free(&r);
                strlcpy(s_error, "Couldn't reach the fuel price service", sizeof(s_error));
            }
            if (s_scr) {
                render();
            }
        } else {
            fuel_result_free(&r);  // superseded (product/location changed)
        }
        ui_unlock();
    }
    heap_caps_free(job);
}

// Starts a fetch if the cached result doesn't match the current product/location or is stale.
static void ensure_data(bool force)
{
    location_t loc;
    bool have_loc = location_get(&loc);
    float lat = loc.lat, lon = loc.lon;
    strlcpy(s_place, have_loc ? loc.name : "", sizeof(s_place));
    if (!have_loc) {
        strlcpy(s_error, "Waiting for your location (Weather)", sizeof(s_error));
        return;
    }

    const int product = settings_get_fuel_product();
    bool fresh = s_result.stations && s_result.product == product &&
                 fuel_distance_km(lat, lon, s_result.lat, s_result.lon) < 1.0f &&
                 time(NULL) - s_result.fetched_at < CACHE_MAX_AGE_S;
    if (fresh && !force) {
        return;
    }
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        strlcpy(s_error, "No Wi-Fi connection", sizeof(s_error));
        return;
    }
    fetch_job_t *job = heap_caps_malloc(sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) {
        return;
    }
    *job = (fetch_job_t){ .gen = ++s_gen, .product = product, .lat = lat, .lon = lon };
    s_loading = true;
    s_error[0] = '\0';
    if (s_result.product != product) {
        fuel_result_free(&s_result);  // don't show the old fuel's prices while loading
    }
    if (!net_worker_post(fetch_job, job)) {
        heap_caps_free(job);
        s_loading = false;
        strlcpy(s_error, "Busy, please try again", sizeof(s_error));
    }
}

// ---------------------------------------------------------------------------
// Detail popup

static void on_popup_close(lv_event_t *e)
{
    lv_obj_delete(lv_event_get_user_data(e));
}

// Universal Google Maps directions link: opens turn-by-turn navigation in the Maps app
// (Android/iOS) or the browser. https://developers.google.com/maps/documentation/urls
static void maps_url(char *buf, size_t len, const fuel_station_t *s)
{
    snprintf(buf, len, "https://www.google.com/maps/dir/?api=1&destination=%.6f,%.6f&travelmode=driving",
             s->lat, s->lon);
}

static void show_detail(const fuel_station_t *s)
{
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

    // Left: station details
    lv_obj_t *info = plain(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_style_pad_row(info, 10, 0);

    char buf[160], p[16], d[16];
    lv_obj_t *brand = ui_label_create(info, ui_font_lg, UI_COLOR_TEXT, s->brand);
    lv_obj_set_width(brand, lv_pct(100));
    lv_label_set_long_mode(brand, LV_LABEL_LONG_WRAP);
    fmt_price(p, sizeof(p), s->price);
    snprintf(buf, sizeof(buf), "%s €/L", p);
    ui_label_create(info, ui_font_xl, UI_COLOR_OK, buf);
    ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, fuel_product(s_result.product)->name);
    lv_obj_t *addr = ui_label_create(info, ui_font_md, UI_COLOR_TEXT, s->address);
    lv_obj_set_width(addr, lv_pct(100));
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    fmt_dist(d, sizeof(d), s->dist_km);
    snprintf(buf, sizeof(buf), "%s  ·  %s away", s->town, d);
    ui_label_create(info, ui_font_md, UI_COLOR_MUTED, buf);
    snprintf(buf, sizeof(buf), "Hours: %s", s->hours[0] ? s->hours : "not published");
    lv_obj_t *hours = ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, buf);
    lv_obj_set_width(hours, lv_pct(100));
    lv_label_set_long_mode(hours, LV_LABEL_LONG_WRAP);
    ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, "Tap anywhere to close");

    // Right: QR code with a Google Maps navigation link
    lv_obj_t *qr_col = plain(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qr_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qr_col, 10, 0);

    char url[160];
    maps_url(url, sizeof(url), s);
    lv_obj_t *qr = lv_qrcode_create(qr_col);
    lv_qrcode_set_size(qr, 230);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    if (lv_qrcode_update(qr, url, strlen(url)) != LV_RESULT_OK) {
        ui_label_create(qr_col, ui_font_sm, UI_COLOR_ERR, "QR code error");
    }
    lv_obj_set_clickable(qr, false);
    ui_label_create(qr_col, ui_font_md, UI_COLOR_TEXT, LV_SYMBOL_GPS "  Scan to navigate");
    ui_label_create(qr_col, ui_font_sm, UI_COLOR_MUTED, "Opens Google Maps on your phone");
}

static void on_row(lv_event_t *e)
{
    show_detail(lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------------
// Rendering

static void summary_item(lv_obj_t *parent, const char *title, const fuel_station_t *s, lv_color_t color, bool big)
{
    char p[16], d[16], buf[96];
    ui_label_create(parent, &lv_font_montserrat_14, UI_COLOR_MUTED, title);
    lv_obj_t *row = plain(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    fmt_price(p, sizeof(p), s->price);
    ui_label_create(row, big ? ui_font_xl : ui_font_lg, color, p);
    ui_label_create(row, ui_font_sm, UI_COLOR_MUTED, "€/L");
    fmt_dist(d, sizeof(d), s->dist_km);
    snprintf(buf, sizeof(buf), "%s  ·  %s", s->brand, d);
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_TEXT, buf);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

static void render_summary(const fuel_station_t **st, int n, float lo, float hi)
{
    lv_obj_clean(s_summary);
    char buf[96], a[16], b[16];
    const int radius = settings_get_fuel_radius();

    if (n == 0) {
        if (s_loading) {
            strlcpy(buf, "Loading prices...", sizeof(buf));
        } else if (s_error[0]) {
            strlcpy(buf, s_error, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "No stations within %d km", radius);
        }
        lv_obj_t *l = ui_label_create(s_summary, ui_font_md, s_error[0] ? UI_COLOR_ERR : UI_COLOR_MUTED, buf);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }

    const fuel_station_t *cheapest = st[0], *nearest = st[0];
    float sum = 0;
    for (int i = 0; i < n; i++) {
        if (st[i]->price < cheapest->price ||
                (st[i]->price == cheapest->price && st[i]->dist_km < cheapest->dist_km)) {
            cheapest = st[i];
        }
        if (st[i]->dist_km < nearest->dist_km) {
            nearest = st[i];
        }
        sum += st[i]->price;
    }
    snprintf(buf, sizeof(buf), "CHEAPEST WITHIN %d KM", radius);
    summary_item(s_summary, buf, cheapest, UI_COLOR_OK, true);
    lv_obj_t *sep = lv_obj_create(s_summary);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    summary_item(s_summary, "NEAREST", nearest, price_color(nearest->price, lo, hi), false);

    ui_label_create(s_summary, &lv_font_montserrat_14, UI_COLOR_MUTED, "AVERAGE NEARBY");
    fmt_price(a, sizeof(a), sum / n);
    snprintf(buf, sizeof(buf), "%s €/L", a);
    ui_label_create(s_summary, ui_font_lg, UI_COLOR_TEXT, buf);
    fmt_price(a, sizeof(a), lo);
    fmt_price(b, sizeof(b), hi);
    snprintf(buf, sizeof(buf), "Range %s – %s\nSpread: %.0f € on a 50 L fill", a, b, (hi - lo) * 50);
    lv_obj_t *range = ui_label_create(s_summary, ui_font_sm, UI_COLOR_MUTED, buf);
    lv_obj_set_width(range, lv_pct(100));
    lv_label_set_long_mode(range, LV_LABEL_LONG_WRAP);

    lv_obj_t *spacer = plain(s_summary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(spacer, 1);
    snprintf(buf, sizeof(buf), "Prices %.16s  ·  Ministerio", s_result.updated);
    ui_label_create(s_summary, ui_font_sm, UI_COLOR_MUTED, buf);
}

static void render_list(const fuel_station_t **st, int n, float lo, float hi)
{
    char buf[128], p[16], d[16];
    if (n > MAX_ROWS) {
        snprintf(buf, sizeof(buf), "%d stations  (showing %d)", n, MAX_ROWS);
    } else {
        snprintf(buf, sizeof(buf), "%d station%s", n, n == 1 ? "" : "s");
    }
    lv_label_set_text(s_list_title, s_loading && n == 0 ? "Loading..." : buf);
    lv_obj_clean(s_list);
    if (n == 0) {
        if (s_loading) {
            lv_obj_t *sp = lv_spinner_create(s_list);
            lv_obj_set_size(sp, 56, 56);
        }
        return;
    }
    for (int i = 0; i < n && i < MAX_ROWS; i++) {
        const fuel_station_t *s = st[i];
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, lv_pct(100), 64);
        lv_obj_set_style_bg_color(row, UI_COLOR_CARD_HI, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)s);

        fmt_price(p, sizeof(p), s->price);
        lv_obj_t *price = ui_label_create(row, ui_font_lg, price_color(s->price, lo, hi), p);
        lv_obj_set_width(price, 96);

        lv_obj_t *mid = plain(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(mid, 1);
        lv_obj_t *brand = ui_label_create(mid, ui_font_md, UI_COLOR_TEXT, s->brand);
        lv_obj_set_size(brand, lv_pct(100), lv_font_get_line_height(ui_font_md));
        lv_label_set_long_mode(brand, LV_LABEL_LONG_DOT);
        snprintf(buf, sizeof(buf), "%s, %s", s->address, s->town);
        lv_obj_t *addr = ui_label_create(mid, ui_font_sm, UI_COLOR_MUTED, buf);
        lv_obj_set_size(addr, lv_pct(100), lv_font_get_line_height(ui_font_sm));  // one line, "..."
        lv_label_set_long_mode(addr, LV_LABEL_LONG_DOT);

        lv_obj_t *right = plain(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        fmt_dist(d, sizeof(d), s->dist_km);
        ui_label_create(right, ui_font_md, UI_COLOR_TEXT, d);
        if (s->h24) {
            ui_label_create(right, &lv_font_montserrat_14, UI_COLOR_OK, "24h");
        }
    }
}

static void render(void)
{
    static const fuel_station_t *st[MAX_VISIBLE];
    const int product = settings_get_fuel_product();
    for (int i = 0; i < FUEL_PRODUCT_COUNT; i++) {
        chip_set(s_product_btns[i], FUEL_PRODUCTS[i].id == product);
    }
    const int radius = settings_get_fuel_radius();
    for (int i = 0; i < 3; i++) {
        chip_set(s_radius_btns[i], RADII[i] == radius);
    }
    chip_set(s_sort_btns[0], s_sort_by_price);
    chip_set(s_sort_btns[1], !s_sort_by_price);

    int n = visible_stations(st, MAX_VISIBLE);
    float lo = 1e9f, hi = 0;
    for (int i = 0; i < n; i++) {
        lo = fminf(lo, st[i]->price);
        hi = fmaxf(hi, st[i]->price);
    }
    render_summary(st, n, lo, hi);
    render_list(st, n, lo, hi);
}

// ---------------------------------------------------------------------------
// Events

static void on_product(lv_event_t *e)
{
    const fuel_product_t *p = lv_event_get_user_data(e);
    if (p->id != settings_get_fuel_product()) {
        settings_set_fuel_product(p->id);
        ensure_data(false);
        render();
    }
}

static void on_radius(lv_event_t *e)
{
    settings_set_fuel_radius((int)(intptr_t)lv_event_get_user_data(e));
    render();
}

static void on_sort(lv_event_t *e)
{
    s_sort_by_price = (intptr_t)lv_event_get_user_data(e) == 0;
    render();
}

static void on_refresh(lv_event_t *e)
{
    ensure_data(true);
    render();
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
    s_scr = NULL;  // results of an in-flight fetch are still cached for next time
}

// ---------------------------------------------------------------------------

// Background refresh on the network worker (every 30 min, or when the fuel type or location
// changed), so the home Fuel card can show the nearest station without opening the app.
static void fuel_bg_step(void)
{
    static TickType_t last;
    static bool tried;
    location_t loc;
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED || !location_get(&loc)) {
        return;
    }
    const int product = settings_get_fuel_product();
    bool stale = !tried || xTaskGetTickCount() - last >= pdMS_TO_TICKS(CACHE_MAX_AGE_S * 1000);
    if (ui_lock()) {
        stale = stale || !s_result.stations || s_result.product != product ||
                fuel_distance_km(loc.lat, loc.lon, s_result.lat, s_result.lon) >= 1.0f;
        ui_unlock();
    }
    if (!stale) {
        return;
    }
    tried = true;
    last = xTaskGetTickCount();
    fuel_result_t r;
    if (fuel_fetch(product, loc.lat, loc.lon, &r) != ESP_OK) {
        return;
    }
    if (ui_lock()) {
        if (!s_loading) {  // a screen-initiated fetch is in flight: let it win
            fuel_result_free(&s_result);
            s_result = r;
            r.stations = NULL;
            if (s_scr) {
                render();
            }
        }
        ui_unlock();
    }
    fuel_result_free(&r);
}

void ui_fuel_init(void)
{
    net_worker_register_step(fuel_bg_step);
}

bool ui_fuel_nearest(fuel_station_t *out, int *product, time_t *fetched_at)
{
    if (!s_result.stations || s_result.count == 0) {
        return false;
    }
    *out = s_result.stations[0];  // stations are sorted by distance
    *product = s_result.product;
    *fetched_at = s_result.fetched_at;
    return true;
}

bool ui_fuel_tile_text(char *buf, size_t len)
{
    if (!s_result.stations || s_result.count == 0) {
        return false;
    }
    const int radius = settings_get_fuel_radius();
    float lo = 1e9f;
    for (int i = 0; i < s_result.count; i++) {
        if (s_result.stations[i].dist_km <= radius) {
            lo = fminf(lo, s_result.stations[i].price);
        }
    }
    if (lo > 1e8f) {
        return false;
    }
    char p[16];
    fmt_price(p, sizeof(p), lo);
    snprintf(buf, len, "%s from %s €", fuel_product(s_result.product)->label, p);
    return true;
}

void ui_fuel_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);

    ensure_data(false);

    char title[128];
    snprintf(title, sizeof(title), s_place[0] ? "Fuel  ·  %s" : "Fuel", s_place);
    lv_obj_t *hdr = ui_header_create(scr, title, on_back);
    ui_header_button(hdr, LV_SYMBOL_REFRESH, NULL, on_refresh, NULL);

    // Fuel type + radius chips
    lv_obj_t *chips = plain(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(chips, 1024 - 2 * UI_PAD, CHIPS_H);
    lv_obj_set_pos(chips, UI_PAD, TOP_Y);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips, 10, 0);
    for (int i = 0; i < FUEL_PRODUCT_COUNT; i++) {
        s_product_btns[i] = chip(chips, FUEL_PRODUCTS[i].label, on_product, (void *)&FUEL_PRODUCTS[i]);
    }
    lv_obj_t *gap = plain(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(gap, 1);
    for (int i = 0; i < 3; i++) {
        char t[12];
        snprintf(t, sizeof(t), "%d km", RADII[i]);
        s_radius_btns[i] = chip(chips, t, on_radius, (void *)(intptr_t)RADII[i]);
    }

    // Summary
    s_summary = ui_card_create(scr);
    lv_obj_set_size(s_summary, LEFT_W, BODY_H);
    lv_obj_set_pos(s_summary, UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(s_summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_summary, 6, 0);
    lv_obj_set_scrollable(s_summary, false);

    // Station list
    lv_obj_t *right = ui_card_create(scr);
    lv_obj_set_size(right, RIGHT_W, BODY_H);
    lv_obj_set_pos(right, LEFT_W + 2 * UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_set_scrollable(right, false);

    lv_obj_t *head = plain(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    s_list_title = ui_label_create(head, ui_font_md, UI_COLOR_TEXT, "");
    lv_obj_set_flex_grow(s_list_title, 1);
    s_sort_btns[0] = chip(head, "Cheapest", on_sort, (void *)0);
    s_sort_btns[1] = chip(head, "Nearest", on_sort, (void *)1);

    s_list = ui_list_create(right);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    render();
    ui_screen_load(scr);
}
