// Traffic: DGT road incidents near the car (accidents, closures, queues, roadworks).
#include <stdio.h>
#include <string.h>
#include "net/wifi_mgr.h"
#include "services/location.h"
#include "services/traffic.h"
#include "services/weather.h"
#include "esp_attr.h"
#include "ui.h"

#define MAX_ROWS   20
#define TOP_Y      (UI_HEADER_H + 4)
#define CHIPS_H    52
#define BODY_Y     (TOP_Y + CHIPS_H + 12)
#define BODY_H     (600 - BODY_Y - UI_PAD)
#define LEFT_W     340
#define RIGHT_W    (1024 - LEFT_W - 3 * UI_PAD)

static const int RADII[] = { 25, 50, 100 };

static lv_obj_t *s_scr;
static lv_obj_t *s_summary;
static lv_obj_t *s_list;
static lv_obj_t *s_list_title;
static lv_obj_t *s_radius_btns[3];
static lv_obj_t *s_filter_btns[2];
static int s_radius = 50;          // km (session only)
static bool s_major_only = true;
EXT_RAM_BSS_ATTR static traffic_item_t s_rows[MAX_ROWS];   // backing store for row user data (LVGL task only)

lv_color_t ui_traffic_color(int sev)
{
    return sev >= TRAFFIC_HIGH ? UI_COLOR_ERR : sev >= TRAFFIC_MEDIUM ? lv_color_hex(0xFB923C) : lv_color_hex(0xFACC15);
}

void ui_traffic_where(char *buf, size_t len, const traffic_item_t *t)
{
    // "A-68 km 246.5 · Zaragoza · eastbound"
    snprintf(buf, len, "%s%s%s%s%s%s%s", t->road[0] ? t->road : "Road", t->km[0] ? " km " : "", t->km,
             t->place[0] ? "  ·  " : "", t->place, t->dir[0] ? "  ·  " : "", t->dir);
}

static void fmt_dist(char *buf, size_t len, float km)
{
    snprintf(buf, len, km < 10 ? "%.1f km" : "%.0f km", km);
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

static lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t v)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 44);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)v);
    lv_obj_center(ui_label_create(b, ui_font_sm, UI_COLOR_TEXT, text));
    return b;
}

static void chip_set(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_HI, 0);
}

static lv_obj_t *sev_dot(lv_obj_t *parent, int sev, int32_t d)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, ui_traffic_color(sev), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

// ---------------------------------------------------------------------------
// Detail popup

static void on_popup_close(lv_event_t *e)
{
    lv_obj_delete(lv_event_get_user_data(e));
}

static void show_detail(const traffic_item_t *t)
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

    lv_obj_t *info = plain(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_style_pad_row(info, 10, 0);
    char buf[160], a[24], b[24];

    lv_obj_t *head = plain(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    sev_dot(head, t->sev, 20);
    ui_label_create(head, ui_font_lg, UI_COLOR_TEXT, t->title);
    if (t->detail[0]) {
        ui_label_create(info, ui_font_md, UI_COLOR_TEXT, t->detail);
    }
    ui_traffic_where(buf, sizeof(buf), t);
    lv_obj_t *where = ui_label_create(info, ui_font_md, UI_COLOR_TEXT, buf);
    lv_obj_set_width(where, lv_pct(100));
    lv_label_set_long_mode(where, LV_LABEL_LONG_WRAP);
    fmt_dist(a, sizeof(a), t->dist_km);
    snprintf(buf, sizeof(buf), "%s away (straight line)", a);
    ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, buf);
    if (t->start) {
        ui_fmt_local(a, sizeof(a), "%a %d %b %H:%M", t->start, weather_utc_offset());
        if (t->end) {
            ui_fmt_local(b, sizeof(b), "%a %d %b %H:%M", t->end, weather_utc_offset());
            snprintf(buf, sizeof(buf), "From %s until %s", a, b);
        } else {
            snprintf(buf, sizeof(buf), "Since %s", a);
        }
        ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, buf);
    }
    ui_label_create(info, ui_font_sm, UI_COLOR_MUTED, "Source: DGT  ·  tap anywhere to close");

    lv_obj_t *qr_col = plain(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qr_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qr_col, 10, 0);
    char url[128];
    snprintf(url, sizeof(url), "https://www.google.com/maps/search/?api=1&query=%.6f,%.6f", t->lat, t->lon);
    lv_obj_t *qr = lv_qrcode_create(qr_col);
    lv_qrcode_set_size(qr, 200);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_clickable(qr, false);
    ui_label_create(qr_col, ui_font_sm, UI_COLOR_MUTED, "Open in Maps");
}

static void on_row(lv_event_t *e)
{
    show_detail(lv_event_get_user_data(e));
}

void ui_traffic_show_detail(const traffic_item_t *t)
{
    show_detail(t);
}

// ---------------------------------------------------------------------------
// Rendering

static void render(void)
{
    for (int i = 0; i < 3; i++) {
        chip_set(s_radius_btns[i], RADII[i] == s_radius);
    }
    chip_set(s_filter_btns[0], s_major_only);
    chip_set(s_filter_btns[1], !s_major_only);

    EXT_RAM_BSS_ATTR static traffic_item_t all[200];  // ~34 KB: keep out of internal RAM
    time_t updated = 0;
    const int n_all = traffic_get(all, 200, s_radius, TRAFFIC_LOW, &updated);
    int n = 0, by_sev[4] = { 0 };
    traffic_item_t nearest_major;
    bool have_major = false;
    for (int i = 0; i < n_all; i++) {
        by_sev[all[i].sev]++;
        if (!have_major && all[i].sev >= TRAFFIC_MEDIUM) {  // list is nearest-first
            nearest_major = all[i];
            have_major = true;
        }
        if (!s_major_only || all[i].sev >= TRAFFIC_MEDIUM) {
            all[n++] = all[i];  // compact in place (n <= i)
        }
    }

    // Summary
    lv_obj_clean(s_summary);
    char buf[128], t[16];
    location_t loc;
    if (!traffic_ready()) {
        lv_obj_t *l = ui_label_create(s_summary, ui_font_md, UI_COLOR_MUTED,
                                      wifi_mgr_state() == WIFI_MGR_CONNECTED ? "Loading DGT incidents..." : "No Wi-Fi connection");
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    } else {
        snprintf(buf, sizeof(buf), "WITHIN %d KM", s_radius);
        ui_label_create(s_summary, &lv_font_montserrat_14, UI_COLOR_MUTED, buf);
        const struct { int sev; const char *name; } rows[] = {
            { TRAFFIC_HIGH, "serious (accident, closure)" },
            { TRAFFIC_MEDIUM, "moderate (queue, obstacle)" },
            { TRAFFIC_LOW, "minor (roadworks, lanes)" },
        };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *r = plain(s_summary, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(r, 12, 0);
            sev_dot(r, rows[i].sev, 14);
            snprintf(t, sizeof(t), "%d", by_sev[rows[i].sev]);
            ui_label_create(r, ui_font_lg, UI_COLOR_TEXT, t);
            ui_label_create(r, ui_font_sm, UI_COLOR_MUTED, rows[i].name);
        }
        const traffic_item_t *nearest = have_major ? &nearest_major : NULL;
        if (nearest) {
            ui_label_create(s_summary, &lv_font_montserrat_14, UI_COLOR_MUTED, "NEAREST MAJOR");
            ui_label_create(s_summary, ui_font_md, ui_traffic_color(nearest->sev), nearest->title);
            char d[16];
            fmt_dist(d, sizeof(d), nearest->dist_km);
            ui_traffic_where(buf, sizeof(buf), nearest);
            lv_obj_t *w = ui_label_create(s_summary, ui_font_sm, UI_COLOR_TEXT, buf);
            lv_obj_set_width(w, lv_pct(100));
            lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
            ui_label_create(s_summary, ui_font_sm, UI_COLOR_MUTED, d);
        }
        lv_obj_t *spacer = plain(s_summary, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_grow(spacer, 1);
        if (location_get(&loc)) {
            snprintf(buf, sizeof(buf), "Distances from %s location", location_source_name(loc.source));
            lv_obj_t *l = ui_label_create(s_summary, ui_font_sm, UI_COLOR_MUTED, buf);
            lv_obj_set_width(l, lv_pct(100));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        }
        ui_fmt_local(t, sizeof(t), "%H:%M", updated, weather_utc_offset());
        snprintf(buf, sizeof(buf), "Updated %s  ·  DGT", t);
        ui_label_create(s_summary, ui_font_sm, UI_COLOR_MUTED, buf);
    }

    // List
    if (n > MAX_ROWS) {
        snprintf(buf, sizeof(buf), "%d incidents  (nearest %d)", n, MAX_ROWS);
    } else {
        snprintf(buf, sizeof(buf), n == 1 ? "1 incident" : "%d incidents", n);
    }
    lv_label_set_text(s_list_title, traffic_ready() ? buf : "");
    lv_obj_clean(s_list);
    if (traffic_ready() && n == 0) {
        ui_list_add_text(s_list, s_major_only ? "No major incidents. Tap \"All\" to include roadworks."
                                              : "No incidents in this radius.");
    }
    for (int i = 0; i < n && i < MAX_ROWS; i++) {
        s_rows[i] = all[i];
        const traffic_item_t *it = &s_rows[i];
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, lv_pct(100), 64);
        lv_obj_set_style_bg_color(row, UI_COLOR_CARD_HI, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)it);

        sev_dot(row, it->sev, 16);
        lv_obj_t *mid = plain(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(mid, 1);
        snprintf(buf, sizeof(buf), it->detail[0] ? "%s  ·  %s" : "%s", it->title, it->detail);
        lv_obj_t *title = ui_label_create(mid, ui_font_md, UI_COLOR_TEXT, buf);
        lv_obj_set_size(title, lv_pct(100), lv_font_get_line_height(ui_font_md));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        ui_traffic_where(buf, sizeof(buf), it);
        lv_obj_t *where = ui_label_create(mid, ui_font_sm, UI_COLOR_MUTED, buf);
        lv_obj_set_size(where, lv_pct(100), lv_font_get_line_height(ui_font_sm));
        lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);

        lv_obj_t *right = plain(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        char d[16];
        fmt_dist(d, sizeof(d), it->dist_km);
        ui_label_create(right, ui_font_md, UI_COLOR_TEXT, d);
        if (it->start) {
            ui_fmt_local(t, sizeof(t), "since %H:%M", it->start, weather_utc_offset());
            if (time(NULL) - it->start > 24 * 3600) {
                ui_fmt_local(t, sizeof(t), "since %d %b", it->start, weather_utc_offset());
            }
            ui_label_create(right, &lv_font_montserrat_14, UI_COLOR_MUTED, t);
        }
    }
}

// ---------------------------------------------------------------------------
// Events

static void on_traffic(void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr) {
        render();
    }
    ui_unlock();
}

static void on_radius(lv_event_t *e)
{
    s_radius = (int)(intptr_t)lv_event_get_user_data(e);
    render();
}

static void on_filter(lv_event_t *e)
{
    s_major_only = (intptr_t)lv_event_get_user_data(e) == 0;
    render();
}

static void on_refresh(lv_event_t *e)
{
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        ui_toast("No Wi-Fi connection");
        return;
    }
    ui_toast("Updating...");
    traffic_refresh();
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
    traffic_remove_listener(on_traffic, NULL);
    s_scr = NULL;
}

void ui_traffic_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *hdr = ui_header_create(scr, "Traffic", on_back);
    ui_header_button(hdr, LV_SYMBOL_REFRESH, NULL, on_refresh, NULL);

    lv_obj_t *chips = plain(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(chips, 1024 - 2 * UI_PAD, CHIPS_H);
    lv_obj_set_pos(chips, UI_PAD, TOP_Y);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips, 10, 0);
    s_filter_btns[0] = chip(chips, "Major", on_filter, 0);
    s_filter_btns[1] = chip(chips, "All (incl. roadworks)", on_filter, 1);
    lv_obj_t *gap = plain(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(gap, 1);
    for (int i = 0; i < 3; i++) {
        char t[12];
        snprintf(t, sizeof(t), "%d km", RADII[i]);
        s_radius_btns[i] = chip(chips, t, on_radius, RADII[i]);
    }

    s_summary = ui_card_create(scr);
    lv_obj_set_size(s_summary, LEFT_W, BODY_H);
    lv_obj_set_pos(s_summary, UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(s_summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_summary, 8, 0);
    lv_obj_set_scrollable(s_summary, false);

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

    render();
    traffic_add_listener(on_traffic, NULL);
    ui_screen_load(scr);
}
