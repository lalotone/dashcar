// Weather app: current conditions, next 24 hours, 7-day forecast, city search.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi_mgr.h"
#include "services/net_worker.h"
#include "services/weather.h"
#include "settings.h"
#include "ui.h"
#include "wx_icon.h"

#define BODY_H   (600 - UI_HEADER_H - UI_PAD)
#define LEFT_W   380
#define RIGHT_W  (1024 - LEFT_W - 3 * UI_PAD)
#define HOURLY_H 196

static lv_obj_t *s_scr;
static lv_obj_t *s_body;
static lv_obj_t *s_units_btn_label;
static weather_t *s_w;   // PSRAM copy used while rendering (LVGL task only)

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

static void detail(lv_obj_t *grid, const char *name, const char *value)
{
    lv_obj_t *cell = plain(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(cell, lv_pct(32));
    lv_obj_set_style_pad_row(cell, 2, 0);
    ui_label_create(cell, ui_font_sm, UI_COLOR_MUTED, name);
    ui_label_create(cell, ui_font_md, UI_COLOR_TEXT, value);
}

static const char *compass(int deg)
{
    static const char *dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return dirs[((deg % 360) + 22) / 45 % 8];
}

static void on_open_radar(lv_event_t *e)
{
    ui_radar_open();
}

// "Next 2 hours" precipitation (15-minute slots), like the iPhone's rain summary.
static void render_rain_block(lv_obj_t *card, const weather_t *w)
{
    if (w->rain15_count == 0) {
        return;
    }
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, UI_COLOR_CARD_HI, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_add_event_cb(box, on_open_radar, LV_EVENT_CLICKED, NULL);  // tap -> radar map

    char buf[64];
    weather_rain_summary(w, buf, sizeof(buf));
    const float thr = weather_rain_threshold(w);
    bool any = false;
    for (int i = 0; i < w->rain15_count; i++) {
        any |= w->rain15[i] >= thr;
    }
    lv_obj_t *head = plain(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *txt = ui_label_create(head, ui_font_sm, any ? lv_color_hex(0x7DD3FC) : UI_COLOR_TEXT, buf);
    lv_obj_set_flex_grow(txt, 1);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
    ui_label_create(head, &lv_font_montserrat_14, UI_COLOR_MUTED, "MAP " LV_SYMBOL_RIGHT);

    // Bars: height ~ intensity, capped at ~4 mm/h.
    const float cap = w->imperial ? 0.04f : 1.0f;
    lv_obj_t *bars = plain(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(bars, lv_pct(100), 34);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < w->rain15_count; i++) {
        float v = w->rain15[i];
        lv_obj_t *b = lv_obj_create(bars);
        lv_obj_remove_style_all(b);
        lv_obj_set_clickable(b, false);
        int h = v >= thr ? LV_MAX(6, (int)(fminf(v / cap, 1.0f) * 34)) : 3;
        lv_obj_set_size(b, lv_pct(11), h);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, v >= thr ? lv_color_hex(0x38BDF8) : UI_COLOR_BORDER, 0);
    }
    lv_obj_t *axis = plain(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(axis, lv_pct(100));
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label_create(axis, &lv_font_montserrat_14, UI_COLOR_MUTED, "Now");
    ui_label_create(axis, &lv_font_montserrat_14, UI_COLOR_MUTED, "+1h");
    ui_label_create(axis, &lv_font_montserrat_14, UI_COLOR_MUTED, "+2h");
}

static void render_current(lv_obj_t *card, const weather_t *w)
{
    char buf[96], t1[16], t2[16], t3[16];

    lv_obj_t *place = ui_label_create(card, ui_font_md, UI_COLOR_MUTED, w->place);
    lv_label_set_long_mode(place, LV_LABEL_LONG_DOT);
    lv_obj_set_width(place, lv_pct(100));

    lv_obj_t *row = plain(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    wx_icon_create(row, 110, w->now.code, w->now.is_day);
    ui_fmt_temp(buf, sizeof(buf), w->now.temp);
    ui_label_create(row, ui_font_huge, UI_COLOR_TEXT, buf);

    ui_label_create(card, ui_font_lg, UI_COLOR_TEXT, weather_code_text(w->now.code));
    ui_fmt_temp(t1, sizeof(t1), w->now.feels_like);
    if (w->day_count > 0) {
        ui_fmt_temp(t2, sizeof(t2), w->days[0].temp_max);
        ui_fmt_temp(t3, sizeof(t3), w->days[0].temp_min);
        snprintf(buf, sizeof(buf), "Feels like %s  ·  H %s  L %s", t1, t2, t3);
    } else {
        snprintf(buf, sizeof(buf), "Feels like %s", t1);
    }
    ui_label_create(card, ui_font_sm, UI_COLOR_MUTED, buf);

    lv_obj_t *grid = plain(card, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_top(grid, 8, 0);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    snprintf(buf, sizeof(buf), "%.0f%%", w->now.humidity);
    detail(grid, "Humidity", buf);
    snprintf(buf, sizeof(buf), "%.0f %s %s", w->now.wind_speed, w->imperial ? "mph" : "km/h", compass(w->now.wind_dir));
    detail(grid, "Wind", buf);
    if (w->imperial) {
        snprintf(buf, sizeof(buf), "%.2f in", w->now.precipitation);
    } else {
        snprintf(buf, sizeof(buf), "%.1f mm", w->now.precipitation);
    }
    detail(grid, "Rain now", buf);
    if (w->day_count > 0) {
        snprintf(buf, sizeof(buf), "%.0f", w->days[0].uv_max);
        detail(grid, "UV max", buf);
        ui_fmt_local(buf, sizeof(buf), "%H:%M", w->days[0].sunrise, w->utc_offset);
        detail(grid, "Sunrise", buf);
        ui_fmt_local(buf, sizeof(buf), "%H:%M", w->days[0].sunset, w->utc_offset);
        detail(grid, "Sunset", buf);
    }

    lv_obj_t *spacer = plain(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(spacer, 1);
    render_rain_block(card, w);
}

// Rain column: probability, or the amount when the provider has no probability (MET Norway).
// Returns true when it's worth highlighting.
static bool rain_text(char *buf, size_t len, int prob, float amount, bool imperial)
{
    if (prob >= 0) {
        snprintf(buf, len, "%d%%", prob);
        return prob >= 30;
    }
    if (amount < (imperial ? 0.005f : 0.1f)) {
        strlcpy(buf, "—", len);
        return false;
    }
    if (imperial) {
        snprintf(buf, len, "%.2fin", amount);
    } else {
        snprintf(buf, len, amount < 10 ? "%.1fmm" : "%.0fmm", amount);
    }
    return amount >= (imperial ? 0.01f : 0.2f);
}

static void render_hourly(lv_obj_t *card, const weather_t *w)
{
    char title[64] = "Next 24 hours";
    if (w->fetched_at) {
        char t[16];
        ui_fmt_local(t, sizeof(t), "%H:%M", w->fetched_at, w->utc_offset);
        snprintf(title, sizeof(title), "Next 24 hours  ·  updated %s  ·  %s", t,
                 w->source[0] ? w->source : "Open-Meteo");
    }
    ui_label_create(card, ui_font_sm, UI_COLOR_MUTED, title);
    lv_obj_t *strip = lv_obj_create(card);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(strip, 4, 0);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);

    char buf[16];
    for (int i = 0; i < w->hour_count; i++) {
        const weather_hour_t *h = &w->hours[i];
        lv_obj_t *col = plain(strip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(col, 68);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_set_event_bubble(col, true);
        if (i == 0) {
            strcpy(buf, "Now");
        } else {
            ui_fmt_local(buf, sizeof(buf), "%H:%M", h->time, w->utc_offset);
        }
        ui_label_create(col, ui_font_sm, UI_COLOR_MUTED, buf);
        wx_icon_create(col, 44, h->code, h->is_day);
        ui_fmt_temp(buf, sizeof(buf), h->temp);
        ui_label_create(col, ui_font_md, UI_COLOR_TEXT, buf);
        const bool wet = rain_text(buf, sizeof(buf), h->precip_prob, h->precip, w->imperial);
        ui_label_create(col, ui_font_sm, wet ? lv_color_hex(0x60A5FA) : UI_COLOR_MUTED, buf);
    }
}

static void render_daily(lv_obj_t *card, const weather_t *w)
{
    float wmin = 1e9f, wmax = -1e9f;
    for (int i = 0; i < w->day_count; i++) {
        wmin = fminf(wmin, w->days[i].temp_min);
        wmax = fmaxf(wmax, w->days[i].temp_max);
    }

    char buf[16];
    for (int i = 0; i < w->day_count; i++) {
        const weather_day_t *d = &w->days[i];
        lv_obj_t *row = plain(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);

        if (i == 0) {
            strcpy(buf, "Today");
        } else {
            ui_fmt_local(buf, sizeof(buf), "%a", d->date, w->utc_offset);
        }
        lv_obj_t *day = ui_label_create(row, ui_font_md, UI_COLOR_TEXT, buf);
        lv_obj_set_width(day, 80);
        wx_icon_create(row, 36, d->code, true);

        const bool wet = rain_text(buf, sizeof(buf), d->precip_prob, d->precip, w->imperial);
        lv_obj_t *pop = ui_label_create(row, ui_font_sm, wet ? lv_color_hex(0x60A5FA) : UI_COLOR_MUTED, buf);
        lv_obj_set_width(pop, 52);

        ui_fmt_temp(buf, sizeof(buf), d->temp_min);
        lv_obj_t *lo = ui_label_create(row, ui_font_md, UI_COLOR_MUTED, buf);
        lv_obj_set_width(lo, 56);
        lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_height(bar, 8);
        lv_obj_set_flex_grow(bar, 1);
        lv_bar_set_mode(bar, LV_BAR_MODE_RANGE);
        lv_bar_set_range(bar, (int32_t)floorf(wmin * 10), (int32_t)ceilf(wmax * 10));
        lv_bar_set_start_value(bar, (int32_t)(d->temp_min * 10), LV_ANIM_OFF);
        lv_bar_set_value(bar, (int32_t)(d->temp_max * 10), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, UI_COLOR_CARD_HI, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_clickable(bar, false);

        ui_fmt_temp(buf, sizeof(buf), d->temp_max);
        lv_obj_t *hi = ui_label_create(row, ui_font_md, UI_COLOR_TEXT, buf);
        lv_obj_set_width(hi, 56);
    }
}

static void on_retry(lv_event_t *e)
{
    weather_refresh();
}

static void on_pick_city(lv_event_t *e)
{
    ui_city_search_open();
}

static void render(void)
{
    lv_obj_clean(s_body);
    const char *msg = NULL;
    weather_status_t st = weather_status(&msg);

    if (!weather_get(s_w)) {
        lv_obj_t *col = plain(s_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_center(col);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 20, 0);
        if (st == WEATHER_STATUS_ERROR) {
            ui_label_create(col, ui_font_lg, UI_COLOR_ERR, msg);
            lv_obj_t *btns = plain(col, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(btns, 16, 0);
            ui_button_create(btns, LV_SYMBOL_REFRESH "  Retry", UI_COLOR_ACCENT, on_retry, NULL);
            ui_button_create(btns, LV_SYMBOL_GPS "  Pick a city", UI_COLOR_CARD_HI, on_pick_city, NULL);
        } else {
            lv_obj_t *sp = lv_spinner_create(col);
            lv_obj_set_size(sp, 64, 64);
            ui_label_create(col, ui_font_md, UI_COLOR_MUTED,
                            wifi_mgr_state() == WIFI_MGR_CONNECTED ? "Getting the forecast..." : "Waiting for Wi-Fi...");
        }
        return;
    }

    lv_obj_t *left = ui_card_create(s_body);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);
    lv_obj_set_scrollable(left, false);
    render_current(left, s_w);

    lv_obj_t *hourly = ui_card_create(s_body);
    lv_obj_set_size(hourly, RIGHT_W, HOURLY_H);
    lv_obj_set_pos(hourly, LEFT_W + 2 * UI_PAD, 0);
    lv_obj_set_flex_flow(hourly, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hourly, 8, 0);
    lv_obj_set_scrollable(hourly, false);
    render_hourly(hourly, s_w);

    lv_obj_t *daily = ui_card_create(s_body);
    lv_obj_set_size(daily, RIGHT_W, BODY_H - HOURLY_H - UI_PAD);
    lv_obj_set_pos(daily, LEFT_W + 2 * UI_PAD, HOURLY_H + UI_PAD);
    lv_obj_set_flex_flow(daily, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(daily, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(daily, 10, 0);
    lv_obj_set_scrollable(daily, false);
    render_daily(daily, s_w);
}

static void on_weather(weather_status_t st, const char *msg, void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr) {
        if (st == WEATHER_STATUS_ERROR && s_w->valid) {
            ui_toast(msg);  // keep showing the last good forecast
        } else if (st != WEATHER_STATUS_LOADING || !s_w->valid) {
            render();
        }
    }
    ui_unlock();
}

static void update_units_label(void)
{
    lv_label_set_text(s_units_btn_label, settings_get_imperial() ? "°F" : "°C");
}

static void on_units(lv_event_t *e)
{
    settings_set_imperial(!settings_get_imperial());
    update_units_label();
    ui_toast(settings_get_imperial() ? "Switched to °F, mph" : "Switched to °C, km/h");
    weather_refresh();
}

static void on_refresh(lv_event_t *e)
{
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        ui_toast("No Wi-Fi connection");
        return;
    }
    ui_toast("Updating...");
    weather_refresh();
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
    weather_remove_listener(on_weather, NULL);
    s_scr = NULL;
}

void ui_weather_open(void)
{
    if (!s_w) {
        s_w = heap_caps_calloc(1, sizeof(*s_w), MALLOC_CAP_SPIRAM);
    }
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *hdr = ui_header_create(scr, "Weather", on_back);
    ui_header_button(hdr, LV_SYMBOL_IMAGE, "Radar", on_open_radar, NULL);
    ui_header_button(hdr, LV_SYMBOL_GPS, "Location", on_pick_city, NULL);
    lv_obj_t *units = ui_header_button(hdr, NULL, "°C", on_units, NULL);
    s_units_btn_label = lv_obj_get_child(units, 0);
    update_units_label();
    ui_header_button(hdr, LV_SYMBOL_REFRESH, NULL, on_refresh, NULL);

    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, lv_pct(100), BODY_H);
    lv_obj_set_pos(s_body, 0, UI_HEADER_H);
    lv_obj_set_scrollable(s_body, false);

    render();
    weather_add_listener(on_weather, NULL);
    ui_screen_load(scr);
}

// ---------------------------------------------------------------------------
// City search

#define MAX_RESULTS 8
#define KB_H        260

static lv_obj_t *s_search_scr;
static lv_obj_t *s_search_ta;
static lv_obj_t *s_results;
static lv_obj_t *s_kb;
static weather_place_t s_places[MAX_RESULTS];
static uint32_t s_search_gen;

typedef struct {
    uint32_t gen;
    char query[96];
    weather_place_t places[MAX_RESULTS];
} search_job_t;

static void on_place_clicked(lv_event_t *e)
{
    const weather_place_t *p = lv_event_get_user_data(e);
    settings_location_t loc = { .valid = true, .lat = p->lat, .lon = p->lon };
    strlcpy(loc.name, p->name, sizeof(loc.name));
    settings_set_location(&loc);
    weather_refresh();
    ui_weather_open();
}

static void search_job(void *arg)
{
    search_job_t *job = arg;
    int n = weather_search_places(job->query, job->places, MAX_RESULTS);

    if (ui_lock()) {
        if (s_search_scr && job->gen == s_search_gen) {
            lv_obj_clean(s_results);
            if (n < 0) {
                ui_list_add_text(s_results, "Search failed, check the connection");
            } else if (n == 0) {
                ui_list_add_text(s_results, "No places found");
            }
            for (int i = 0; i < n; i++) {
                s_places[i] = job->places[i];
                ui_list_add_button(s_results, LV_SYMBOL_GPS, s_places[i].name, on_place_clicked, &s_places[i]);
            }
        }
        ui_unlock();
    }
    heap_caps_free(job);
}

static void do_search(void)
{
    const char *q = lv_textarea_get_text(s_search_ta);
    if (strlen(q) < 2) {
        ui_toast("Type at least 2 letters");
        return;
    }
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        ui_toast("No Wi-Fi connection");
        return;
    }
    search_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) {
        return;
    }
    job->gen = ++s_search_gen;
    strlcpy(job->query, q, sizeof(job->query));
    lv_obj_clean(s_results);
    ui_list_add_text(s_results, "Searching...");
    if (!net_worker_post(search_job, job)) {
        heap_caps_free(job);
        lv_obj_clean(s_results);
        ui_list_add_text(s_results, "Busy, please try again");
    }
}

static void on_search_clicked(lv_event_t *e)
{
    do_search();
}

static void on_kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        do_search();
    }
}

static void on_use_ip(lv_event_t *e)
{
    settings_set_location(NULL);
    weather_refresh();
    ui_toast("Using your approximate location");
    ui_weather_open();
}

static void on_search_back(lv_event_t *e)
{
    ui_weather_open();
}

static void on_search_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_search_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    s_search_scr = NULL;
    s_search_gen++;  // drop results of in-flight searches
}

void ui_city_search_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_search_scr = scr;
    lv_obj_add_event_cb(scr, on_search_delete, LV_EVENT_DELETE, NULL);
    ui_header_create(scr, "Choose location", on_search_back);

    lv_obj_t *row = plain(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(row, 0, UI_HEADER_H);
    lv_obj_set_style_pad_hor(row, UI_PAD, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_search_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, "City name, e.g. Valencia");
    lv_textarea_set_max_length(s_search_ta, 60);
    lv_obj_set_flex_grow(s_search_ta, 1);
    lv_obj_set_style_text_font(s_search_ta, ui_font_md, 0);
    lv_obj_set_style_bg_color(s_search_ta, UI_COLOR_CARD, 0);
    lv_obj_set_style_pad_ver(s_search_ta, 12, 0);
    lv_obj_add_state(s_search_ta, LV_STATE_FOCUSED);

    ui_button_create(row, LV_SYMBOL_RIGHT "  Search", UI_COLOR_ACCENT, on_search_clicked, NULL);
    ui_button_create(row, LV_SYMBOL_GPS "  My location", UI_COLOR_CARD_HI, on_use_ip, NULL);

    const int32_t list_y = UI_HEADER_H + 72;
    s_results = ui_list_create(scr);
    lv_obj_set_size(s_results, 1024 - 2 * UI_PAD, 600 - KB_H - list_y - 8);
    lv_obj_set_pos(s_results, UI_PAD, list_y);
    settings_location_t cur;
    settings_get_location(&cur);
    char buf[128];
    snprintf(buf, sizeof(buf), "Current: %s", cur.valid ? cur.name : "automatic (by IP address)");
    ui_list_add_text(s_results, buf);

    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, lv_pct(100), KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, ui_font_md, 0);
    lv_keyboard_set_textarea(s_kb, s_search_ta);
    lv_obj_add_event_cb(s_kb, on_kb_event, LV_EVENT_READY, NULL);

    ui_screen_load(scr);
}
