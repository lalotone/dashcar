// Home: glance area (clock, date, weather now, rain soon) + live app cards.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "net/wifi_mgr.h"
#include "services/alerts.h"
#include "services/map_tiles.h"
#include "services/parking.h"
#include "services/rain_now.h"
#include "services/traffic.h"
#include "services/weather.h"
#include "settings.h"
#include "esp_attr.h"
#include "ui.h"
#include "wx_icon.h"

extern const lv_image_dsc_t dacia_logo;

#define LEFT_X    32
#define LEFT_W    440
#define GRID_X    (LEFT_X + LEFT_W + 24)
#define GRID_W    (1024 - GRID_X - 24)
#define TOP_Y     UI_HEADER_H
#define GRID_H    (600 - TOP_Y - 24)
#define ALERTS_W  226   // panel right of the clock
#define ALERTS_H  156
#define GRID_COLS 3   // 3x2 grid of app cards
#define CARD_GAP  12
#define CARD_W    ((GRID_W - (GRID_COLS - 1) * CARD_GAP) / GRID_COLS)
#define CARD_H    ((GRID_H - CARD_GAP) / 2)

#define C_SURFACE lv_color_hex(0x111827)
#define C_EDGE    lv_color_hex(0x1F2A3D)

typedef enum { APP_WEATHER, APP_RADAR, APP_TRAFFIC, APP_FUEL, APP_SETTINGS, APP_PARKING, APP_COUNT } app_id_t;

typedef struct {
    const char *title;
    uint32_t color;
    void (*open)(void);
} app_card_t;

static const app_card_t APPS[APP_COUNT] = {
    [APP_WEATHER] = { "Weather", 0x60A5FA, ui_weather_open },
    [APP_RADAR]   = { "Radar",   0x2DD4BF, ui_radar_open },
    [APP_TRAFFIC] = { "Traffic", 0xF43F5E, ui_traffic_open },
    [APP_FUEL]    = { "Fuel",    0xF59E0B, ui_fuel_open },
    [APP_SETTINGS] = { "Settings", 0xA78BFA, ui_settings_open },
    [APP_PARKING] = { "Parked",  0x34D399, ui_parking_open },
};

static lv_obj_t *s_scr;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_alerts;          // alerts panel body (right of the clock)
static lv_obj_t *s_rain;            // live rain map card
static lv_obj_t *s_rain_canvas;
static lv_obj_t *s_rain_marks;      // marker layer over the canvas
static lv_obj_t *s_rain_tag;        // "RAIN RADAR · 18:00"
static lv_obj_t *s_rain_caption;    // "No rain expected in the next 2 hours"
static lv_obj_t *s_badge[APP_COUNT];
static lv_obj_t *s_sub[APP_COUNT];  // live subtitles
static lv_obj_t *s_fuel_qr;         // QR to the nearest station (replaces the Fuel badge)
static weather_t *s_w;              // PSRAM copy, LVGL task only

// ---------------------------------------------------------------------------
// Small drawn icons (LVGL's symbol font has no fuel pump or radar)

static lv_obj_t *shape(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *ring(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t d, int32_t w, lv_color_t c, lv_opa_t opa)
{
    lv_obj_t *o = shape(parent, cx - d / 2, cy - d / 2, d, d, LV_RADIUS_CIRCLE, c);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    return o;
}

static void icon_fuel(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Pump body with a display window, base, and a hose to the nozzle on the right.
    shape(box, s * 18 / 100, s * 12 / 100, s * 44 / 100, s * 72 / 100, s / 12, c);
    shape(box, s * 25 / 100, s * 20 / 100, s * 30 / 100, s * 20 / 100, 2, C_SURFACE);
    shape(box, s * 12 / 100, s * 80 / 100, s * 56 / 100, s * 10 / 100, 2, c);
    shape(box, s * 62 / 100, s * 30 / 100, s * 16 / 100, s * 7 / 100, 2, c);   // arm
    shape(box, s * 73 / 100, s * 30 / 100, s * 7 / 100, s * 42 / 100, 3, c);   // hose down
    shape(box, s * 73 / 100, s * 14 / 100, s * 7 / 100, s * 20 / 100, 3, c);   // nozzle up
}

static void icon_radar(lv_obj_t *box, int32_t s, lv_color_t c)
{
    ring(box, s / 2, s / 2, s * 92 / 100, 2, c, LV_OPA_50);
    ring(box, s / 2, s / 2, s * 58 / 100, 2, c, LV_OPA_80);
    shape(box, s / 2 - s / 12, s / 2 - s / 12, s / 6, s / 6, LV_RADIUS_CIRCLE, c);
    // Sweep: a short bar from the centre towards the upper right, plus a "blip".
    lv_obj_t *sweep = shape(box, s / 2, s / 2 - 1, s * 44 / 100, 3, 1, c);
    lv_obj_set_style_transform_rotation(sweep, -450, 0);  // -45 degrees
    lv_obj_set_style_transform_pivot_x(sweep, 0, 0);
    lv_obj_set_style_transform_pivot_y(sweep, 1, 0);
    shape(box, s * 70 / 100, s * 60 / 100, s / 8, s / 8, LV_RADIUS_CIRCLE, c);
}

static void icon_parking(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Road-sign style: filled rounded square with the "P" cut out in the surface colour.
    shape(box, 0, 0, s, s, s / 5, c);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, "P");
    lv_obj_set_style_text_font(l, ui_font_lg, 0);
    lv_obj_set_style_text_color(l, C_SURFACE, 0);
    lv_obj_center(l);
}

static void icon_traffic(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Traffic light: housing + red/amber/green lamps.
    shape(box, s * 30 / 100, 0, s * 40 / 100, s, s / 6, c);
    const uint32_t lamps[] = { 0xEF4444, 0xFACC15, 0x22C55E };
    for (int i = 0; i < 3; i++) {
        shape(box, s * 38 / 100, s * (6 + i * 31) / 100, s * 24 / 100, s * 24 / 100, LV_RADIUS_CIRCLE,
              lv_color_hex(lamps[i]));
    }
}

static void fill_badge(app_id_t id)
{
    lv_obj_t *b = s_badge[id];
    lv_obj_clean(b);
    const lv_color_t c = lv_color_hex(APPS[id].color);
    const int32_t s = 34;
    lv_obj_t *box = lv_obj_create(b);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, s, s);
    lv_obj_center(box);
    lv_obj_set_clickable(box, false);
    switch (id) {
    case APP_WEATHER:
        lv_obj_delete(box);
        wx_icon_create(b, 44, s_w->valid ? s_w->now.code : 2, s_w->valid ? s_w->now.is_day : true);
        lv_obj_center(lv_obj_get_child(b, 0));
        break;
    case APP_RADAR:
        icon_radar(box, s, c);
        break;
    case APP_TRAFFIC:
        icon_traffic(box, s, c);
        break;
    case APP_FUEL:
        icon_fuel(box, s, c);
        break;
    case APP_PARKING:
        icon_parking(box, s, c);
        break;
    case APP_SETTINGS: {
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, c, 0);
        lv_obj_center(l);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Live content

// Fuel card: QR (Google Maps directions) to the nearest station + "BRAND / price · distance".
static void update_fuel_card(void)
{
    static double shown_lat, shown_lon;   // what the QR currently encodes
    fuel_station_t st;
    int product;
    time_t fetched;
    if (!ui_fuel_nearest(&st, &product, &fetched)) {
        ui_set_hidden_if_changed(s_fuel_qr, true);
        ui_set_hidden_if_changed(s_badge[APP_FUEL], false);
        ui_label_set_text_if_changed(s_sub[APP_FUEL], "Nearest\nstation");
        shown_lat = shown_lon = 0;
        return;
    }
    if (st.lat != shown_lat || st.lon != shown_lon || lv_obj_is_hidden(s_fuel_qr)) {
        // Short URL form keeps the QR at ~3 px per module on a 118 px code.
        char url[80];
        snprintf(url, sizeof(url), "https://www.google.com/maps/dir//%.5f,%.5f", st.lat, st.lon);
        lv_qrcode_update(s_fuel_qr, url, strlen(url));
        shown_lat = st.lat;
        shown_lon = st.lon;
    }
    ui_set_hidden_if_changed(s_badge[APP_FUEL], true);
    ui_set_hidden_if_changed(s_fuel_qr, false);
    char price[16], buf[64];
    snprintf(price, sizeof(price), "%.3f", st.price);
    char *dot = strchr(price, '.');
    if (dot) {
        *dot = ',';
    }
    char dist[16];
    if (st.dist_km < 1.0f) {
        snprintf(dist, sizeof(dist), "%dm", (int)(st.dist_km * 1000 / 10) * 10);
    } else {
        snprintf(dist, sizeof(dist), "%.1fkm", st.dist_km);
    }
    snprintf(buf, sizeof(buf), "%.12s\n%s€ · %s", st.brand, price, dist);  // compact: 128 px wide
    ui_label_set_text_if_changed(s_sub[APP_FUEL], buf);
}

// Parked card subtitle: age of the spot ("Demo spot" until a GPS saves real spots).
static void update_parking_card(void)
{
    parking_spot_t p;
    parking_get(&p);
    char buf[48];
    if (p.demo) {
        ui_label_set_text_if_changed(s_sub[APP_PARKING], "Demo spot\nGPS pending");
    } else {
        ui_parking_age(&p, buf, sizeof(buf));
        char *sep = strstr(buf, "  ·  ");  // "2 h ago  ·  Thu 18:05" -> two lines
        if (sep) {
            *sep = '\n';
            memmove(sep + 1, sep + 5, strlen(sep + 5) + 1);
        }
        ui_label_set_text_if_changed(s_sub[APP_PARKING], buf);
    }
}

static void update_subtitles(void)
{
    char buf[64], t[32];
    // Weather / radar from the latest forecast
    if (s_w->valid) {
        ui_fmt_temp(t, sizeof(t), s_w->now.temp);
        snprintf(buf, sizeof(buf), "%s\n%s", t, weather_code_text(s_w->now.code));
        ui_label_set_text_if_changed(s_sub[APP_WEATHER], buf);
        // Radar: short form of the 2 h rain outlook
        if (s_w->rain15_count > 0) {
            weather_rain_summary(s_w, buf, sizeof(buf));
            const char *p;
            // Two short lines fit the 160 px card: "No rain" / "next 2 h".
            if (strncmp(buf, "No rain", 7) == 0) {
                strlcpy(buf, "No rain\nnext 2 h", sizeof(buf));
            } else if ((p = strstr(buf, "starting in ")) != NULL) {
                snprintf(t, sizeof(t), "Rain in\n%s", p + 12);
                strlcpy(buf, t, sizeof(buf));
            } else if ((p = strstr(buf, "stopping in ")) != NULL) {
                snprintf(t, sizeof(t), "Stops in\n%s", p + 12);
                strlcpy(buf, t, sizeof(buf));
            } else {
                strlcpy(buf, "Rain\nnext 2 h", sizeof(buf));
            }
            ui_label_set_text_if_changed(s_sub[APP_RADAR], buf);
        } else {
            ui_label_set_text_if_changed(s_sub[APP_RADAR], "Rain map");
        }
    } else {
        ui_label_set_text_if_changed(s_sub[APP_WEATHER], "Loading...");
        ui_label_set_text_if_changed(s_sub[APP_RADAR], "Rain map");
    }
    // Traffic: major incidents nearby
    if (!traffic_ready()) {
        ui_label_set_text_if_changed(s_sub[APP_TRAFFIC], "DGT incidents");
    } else {
        EXT_RAM_BSS_ATTR static traffic_item_t inc[30];
        int n = traffic_get(inc, 30, 25, TRAFFIC_MEDIUM, NULL);
        if (n == 0) {
            ui_label_set_text_if_changed(s_sub[APP_TRAFFIC], "Nothing major\nnearby");
        } else {
            snprintf(buf, sizeof(buf), "%d major\nnearest %.0f km", n, inc[0].dist_km);
            ui_label_set_text_if_changed(s_sub[APP_TRAFFIC], buf);
        }
    }
    // Fuel from the last search
    update_fuel_card();
    update_parking_card();
    // Settings: Wi-Fi + night mode at a glance
    const char *night = settings_get_night_mode() == NIGHT_MODE_ON ? "night on" :
                        settings_get_night_mode() == NIGHT_MODE_OFF ? "night off" : "night auto";
    switch (wifi_mgr_state()) {
    case WIFI_MGR_CONNECTED:
        snprintf(buf, sizeof(buf), "%s\n%s", wifi_mgr_ssid(), night);
        break;
    case WIFI_MGR_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting...\n%s", night);
        break;
    default:
        snprintf(buf, sizeof(buf), "Offline\nset up Wi-Fi");
        break;
    }
    ui_label_set_text_if_changed(s_sub[APP_SETTINGS], buf);
}

// ---------------------------------------------------------------------------
// Weather warnings (AEMET via MeteoAlarm)

static lv_color_t alert_bg(alert_level_t l)
{
    return l == ALERT_RED ? lv_color_hex(0xEF4444) : l == ALERT_ORANGE ? lv_color_hex(0xFB923C) : lv_color_hex(0xFACC15);
}

// "until 21:59", "until Fri 06:00", "from 15:00"
static void alert_when(char *buf, size_t len, const alert_t *a)
{
    time_t now = time(NULL);
    const int32_t off = weather_utc_offset();
    const bool upcoming = a->onset > now;
    const time_t t = upcoming ? a->onset : a->expires;
    char d1[8], d2[8], hm[8];
    ui_fmt_local(d1, sizeof(d1), "%j", now, off);
    ui_fmt_local(d2, sizeof(d2), "%j", t, off);
    ui_fmt_local(hm, sizeof(hm), "%H:%M", t, off);
    if (strcmp(d1, d2) == 0) {
        snprintf(buf, len, "%s %s", upcoming ? "from" : "until", hm);
    } else {
        char day[8];
        ui_fmt_local(day, sizeof(day), "%a", t, off);
        snprintf(buf, len, "%s %s %s", upcoming ? "from" : "until", day, hm);
    }
}

#define POPUP_TRAFFIC_ROWS 5

static lv_obj_t *s_popup;                                         // alerts popup background
EXT_RAM_BSS_ATTR static traffic_item_t s_popup_inc[POPUP_TRAFFIC_ROWS];  // row user data

static lv_obj_t *popup_section(lv_obj_t *card, const char *title)
{
    lv_obj_t *l = ui_label_create(card, ui_font_sm, UI_COLOR_MUTED, title);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

static void on_popup_incident(lv_event_t *e)
{
    const traffic_item_t *t = lv_event_get_user_data(e);
    lv_obj_delete(s_popup);
    s_popup = NULL;
    ui_traffic_show_detail(t);
}

static void on_popup_open_traffic(lv_event_t *e)
{
    lv_obj_delete(s_popup);
    s_popup = NULL;
    ui_traffic_open();
}

static void on_popup_bg(lv_event_t *e)
{
    lv_obj_delete(s_popup);
    s_popup = NULL;
}

// Everything the home Alerts panel summarises: official warnings + major traffic nearby.
static void show_alerts_popup(void)
{
    alert_t al[ALERTS_MAX];
    const int nw = alerts_get(al, ALERTS_MAX);
    EXT_RAM_BSS_ATTR static traffic_item_t inc[20];
    const int nt = traffic_get(inc, 20, 25, TRAFFIC_MEDIUM, NULL);
    char zone[64], buf[160], a[16], b[16];
    alerts_zone_name(zone, sizeof(zone));

    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_60, 0);
    lv_obj_add_event_cb(s_popup, on_popup_bg, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = ui_card_create(s_popup);
    lv_obj_set_size(card, 780, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 560, 0);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_add_event_cb(card, on_popup_bg, LV_EVENT_CLICKED, NULL);

    ui_label_create(card, ui_font_lg, UI_COLOR_TEXT, LV_SYMBOL_WARNING "  Alerts");

    // Weather warnings (AEMET)
    snprintf(buf, sizeof(buf), "WEATHER WARNINGS  ·  %s", zone[0] ? zone : "your area");
    popup_section(card, buf);
    if (nw == 0) {
        ui_label_create(card, ui_font_md, UI_COLOR_MUTED, "No weather warnings");
    }
    for (int i = 0; i < nw; i++) {
        lv_obj_t *r = lv_obj_create(card);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 14, 0);
        lv_obj_set_clickable(r, false);
        lv_obj_t *dot = lv_obj_create(r);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, alert_bg(al[i].level), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_clickable(dot, false);
        lv_obj_t *col = lv_obj_create(r);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_clickable(col, false);
        ui_label_create(col, ui_font_md, UI_COLOR_TEXT, al[i].title);
        ui_fmt_local(a, sizeof(a), "%a %H:%M", al[i].onset, weather_utc_offset());
        ui_fmt_local(b, sizeof(b), "%a %H:%M", al[i].expires, weather_utc_offset());
        if (strcmp(al[i].area, zone) == 0) {
            snprintf(buf, sizeof(buf), "%s  →  %s", a, b);
        } else {
            snprintf(buf, sizeof(buf), "%s  →  %s  ·  %s", a, b, al[i].area);
        }
        ui_label_create(col, ui_font_sm, UI_COLOR_MUTED, buf);
    }

    // Major road incidents nearby (DGT); each row opens its detail (with Maps QR)
    popup_section(card, "TRAFFIC WITHIN 25 KM  ·  serious and moderate");
    if (nt == 0) {
        ui_label_create(card, ui_font_md, UI_COLOR_MUTED,
                        traffic_ready() ? "No major incidents nearby" : "Loading DGT incidents...");
    }
    for (int i = 0; i < nt && i < POPUP_TRAFFIC_ROWS; i++) {
        s_popup_inc[i] = inc[i];
        const traffic_item_t *t = &s_popup_inc[i];
        lv_obj_t *row = lv_button_create(card);
        lv_obj_set_size(row, lv_pct(100), 56);
        lv_obj_set_style_bg_color(row, UI_COLOR_CARD_HI, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_add_event_cb(row, on_popup_incident, LV_EVENT_CLICKED, (void *)t);
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, ui_traffic_color(t->sev), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_clickable(dot, false);
        snprintf(buf, sizeof(buf), t->detail[0] ? "%s  ·  %s" : "%s", t->title, t->detail);
        lv_obj_t *title = ui_label_create(row, ui_font_md, UI_COLOR_TEXT, buf);
        lv_obj_set_width(title, 300);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        char where[96];
        ui_traffic_where(where, sizeof(where), t);
        lv_obj_t *w = ui_label_create(row, ui_font_sm, UI_COLOR_MUTED, where);
        lv_obj_set_flex_grow(w, 1);
        lv_label_set_long_mode(w, LV_LABEL_LONG_DOT);
        snprintf(buf, sizeof(buf), t->dist_km < 10 ? "%.1f km" : "%.0f km", t->dist_km);
        ui_label_create(row, ui_font_md, UI_COLOR_TEXT, buf);
    }

    lv_obj_t *foot = lv_obj_create(card);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(foot, 6, 0);
    lv_obj_set_clickable(foot, false);
    if (nt > POPUP_TRAFFIC_ROWS) {
        snprintf(buf, sizeof(buf), "+%d more  ·  AEMET via MeteoAlarm, DGT", nt - POPUP_TRAFFIC_ROWS);
    } else {
        snprintf(buf, sizeof(buf), "Sources: AEMET via MeteoAlarm, DGT");
    }
    ui_label_create(foot, ui_font_sm, UI_COLOR_MUTED, buf);
    ui_button_create(foot, LV_SYMBOL_RIGHT "  Open Traffic", UI_COLOR_ACCENT, on_popup_open_traffic, NULL);
}

// ---------------------------------------------------------------------------
// Alerts panel: official warnings first, then major road incidents nearby

static void alert_row(lv_obj_t *parent, lv_color_t dot, const char *title, const char *sub)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_clickable(r, false);
    lv_obj_t *d = lv_obj_create(r);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 10, 10);
    lv_obj_set_style_margin_top(d, 7, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, dot, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_clickable(d, false);
    lv_obj_t *col = lv_obj_create(r);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(col, false);
    lv_obj_t *t = ui_label_create(col, ui_font_sm, UI_COLOR_TEXT, title);
    lv_obj_set_size(t, lv_pct(100), lv_font_get_line_height(ui_font_sm));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_t *u = ui_label_create(col, ui_font_sm, UI_COLOR_MUTED, sub);
    lv_obj_set_size(u, lv_pct(100), lv_font_get_line_height(ui_font_sm));
    lv_label_set_long_mode(u, LV_LABEL_LONG_DOT);
}

static void fill_alerts(void)
{
    lv_obj_clean(s_alerts);
    alert_t al[ALERTS_MAX];
    EXT_RAM_BSS_ATTR static traffic_item_t inc[8];
    const int nw = alerts_get(al, ALERTS_MAX);
    const int nt = traffic_get(inc, 8, 25, TRAFFIC_MEDIUM, NULL);
    ui_label_create(s_alerts, &lv_font_montserrat_14, UI_COLOR_MUTED, "ALERTS");
    if (nw + nt == 0) {
        alert_row(s_alerts, UI_COLOR_OK, "All clear", "No warnings or major incidents");
        return;
    }
    const int max_rows = 2;
    int shown = 0;
    char title[48], sub[64];
    for (int i = 0; i < nw && shown < max_rows; i++, shown++) {
        strlcpy(title, al[i].title, sizeof(title));
        char *w = strstr(title, " Warning");  // "Yellow Wind Warning" -> "Yellow Wind"
        if (w) {
            *w = '\0';
        }
        alert_when(sub, sizeof(sub), &al[i]);
        alert_row(s_alerts, alert_bg(al[i].level), title, sub);
    }
    for (int i = 0; i < nt && shown < max_rows; i++, shown++) {
        snprintf(sub, sizeof(sub), "%s  ·  %.0f km", inc[i].road[0] ? inc[i].road : inc[i].place, inc[i].dist_km);
        alert_row(s_alerts, ui_traffic_color(inc[i].sev), inc[i].title, sub);
    }
    if (nw + nt > shown) {
        snprintf(sub, sizeof(sub), "+%d more", nw + nt - shown);
        ui_label_create(s_alerts, ui_font_sm, UI_COLOR_MUTED, sub);
    }
}

static void on_alerts_tap(lv_event_t *e)
{
    show_alerts_popup();
}

// ---------------------------------------------------------------------------
// Live rain map around the car

static void rain_mark(int32_t x, int32_t y, int32_t d, lv_color_t c, lv_color_t border)
{
    if (x < d || y < d || x > RAIN_NOW_W - d || y > RAIN_NOW_H - d) {
        return;
    }
    lv_obj_t *m = lv_obj_create(s_rain_marks);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, d, d);
    lv_obj_set_pos(m, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(m, c, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m, border, 0);
    lv_obj_set_style_border_width(m, d > 14 ? 3 : 2, 0);
    lv_obj_set_clickable(m, false);
}

static void fill_rain(void)
{
    time_t radar_t = 0;
    const uint16_t *img = rain_now_image(&radar_t);
    lv_obj_clean(s_rain_marks);
    if (!img) {
        ui_set_hidden_if_changed(s_rain_canvas, true);
        ui_label_set_text_if_changed(s_rain_tag, "RAIN RADAR  ·  loading...");
        return;
    }
    lv_canvas_set_buffer(s_rain_canvas, (void *)img, RAIN_NOW_W, RAIN_NOW_H, LV_COLOR_FORMAT_RGB565);
    ui_set_hidden_if_changed(s_rain_canvas, false);
    lv_obj_invalidate(s_rain_canvas);

    int32_t ox, oy;
    rain_now_origin(&ox, &oy);
    EXT_RAM_BSS_ATTR static traffic_item_t inc[30];
    const int n = traffic_get(inc, 30, 80, TRAFFIC_MEDIUM, NULL);
    for (int i = 0; i < n; i++) {
        rain_mark((int32_t)lround(mt_lon_to_px(inc[i].lon, RAIN_NOW_ZOOM)) - ox,
                  (int32_t)lround(mt_lat_to_px(inc[i].lat, RAIN_NOW_ZOOM)) - oy,
                  12, ui_traffic_color(inc[i].sev), lv_color_black());
    }
    rain_mark(RAIN_NOW_W / 2, RAIN_NOW_H / 2, 18, UI_COLOR_ACCENT, lv_color_white());  // you

    char t[16], buf[48];
    if (radar_t) {
        ui_fmt_local(t, sizeof(t), "%H:%M", radar_t, weather_utc_offset());
        snprintf(buf, sizeof(buf), "RAIN RADAR  ·  %s", t);
    } else {
        snprintf(buf, sizeof(buf), "RAIN RADAR  ·  unavailable");
    }
    ui_label_set_text_if_changed(s_rain_tag, buf);
}

static void fill_rain_caption(void)
{
    char buf[64];
    if (s_w->valid && s_w->rain15_count > 0) {
        weather_rain_summary(s_w, buf, sizeof(buf));
    } else {
        strlcpy(buf, "Tap for the full radar", sizeof(buf));
    }
    ui_label_set_text_if_changed(s_rain_caption, buf);
}

static void on_rain_now(void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr) {
        fill_rain();
    }
    ui_unlock();
}

static void on_alerts(void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr) {
        fill_alerts();
        fill_rain();       // incident markers
        update_subtitles();
    }
    ui_unlock();
}

static void refresh_weather(void)
{
    weather_get(s_w);
    fill_rain_caption();
    fill_badge(APP_WEATHER);
    update_subtitles();
}

static void clock_update(lv_timer_t *t)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        ui_label_set_text_if_changed(s_clock, "--:--");
        ui_label_set_text_if_changed(s_date, wifi_mgr_state() == WIFI_MGR_CONNECTED ? "Syncing time..." : "Waiting for Wi-Fi");
    } else {
        char buf[48];
        int32_t off = weather_utc_offset();
        ui_fmt_local(buf, sizeof(buf), "%H:%M", now, off);
        ui_label_set_text_if_changed(s_clock, buf);
        ui_fmt_local(buf, sizeof(buf), "%A, %e %B", now, off);
        ui_label_set_text_if_changed(s_date, buf);
    }
    update_subtitles();  // Wi-Fi state, fuel cache
}

static void on_weather(weather_status_t st, const char *msg, void *ctx)
{
    if (st == WEATHER_STATUS_LOADING || !ui_lock()) {
        return;
    }
    if (s_scr) {
        refresh_weather();
    }
    ui_unlock();
}

static void on_card(lv_event_t *e)
{
    const app_card_t *app = lv_event_get_user_data(e);
    app->open();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    weather_remove_listener(on_weather, NULL);
    alerts_remove_listener(on_alerts, NULL);
    traffic_remove_listener(on_alerts, NULL);
    rain_now_remove_listener(on_rain_now, NULL);
    s_scr = NULL;
    ui_status_set_on_home(false);
}

// ---------------------------------------------------------------------------
// Layout

static void style_pressable(lv_obj_t *o)
{
    // Flat surface; slightly lighter and shrunk while pressed.
    static lv_style_transition_dsc_t tr;
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_BG_COLOR, 0 };
    static bool init;
    if (!init) {
        lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, NULL);
        init = true;
    }
    lv_obj_set_style_bg_color(o, C_SURFACE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, C_EDGE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 24, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_transform_pivot_x(o, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(o, lv_pct(50), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1A2436), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(o, 244, LV_STATE_PRESSED);
    lv_obj_set_style_transition(o, &tr, 0);
    lv_obj_set_style_transition(o, &tr, LV_STATE_PRESSED);
}

static void card_create(lv_obj_t *grid, app_id_t id)
{
    const app_card_t *app = &APPS[id];
    lv_obj_t *card = lv_button_create(grid);
    lv_obj_set_size(card, CARD_W, CARD_H);
    style_pressable(card);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(card, on_card, LV_EVENT_CLICKED, (void *)app);

    // Tinted circular badge
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 56, 56);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(app->color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_set_clickable(badge, false);
    lv_obj_set_scrollable(badge, false);
    s_badge[id] = badge;
    fill_badge(id);
    if (id == APP_FUEL) {
        lv_obj_t *qr = lv_qrcode_create(card);
        lv_qrcode_set_size(qr, 118);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_obj_set_clickable(qr, false);
        lv_obj_set_hidden(qr, true);
        s_fuel_qr = qr;
    }

    lv_obj_t *spacer = lv_obj_create(card);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);

    ui_label_create(card, ui_font_lg, UI_COLOR_TEXT, app->title);
    s_sub[id] = ui_label_create(card, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_set_size(s_sub[id], lv_pct(100), 2 * lv_font_get_line_height(ui_font_sm));  // titles align
    lv_label_set_long_mode(s_sub[id], LV_LABEL_LONG_DOT);
}

static void on_rain_tap(lv_event_t *e)
{
    ui_radar_open();
}

static lv_obj_t *overlay_label(lv_obj_t *parent, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_TEXT, "");
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_radius(l, 12, 0);
    lv_obj_set_style_pad_hor(l, 12, 0);
    lv_obj_set_style_pad_ver(l, 5, 0);
    lv_obj_align(l, align, x, y);
    return l;
}

void ui_home_open(void)
{
    if (!s_w) {
        s_w = heap_caps_calloc(1, sizeof(*s_w), MALLOC_CAP_SPIRAM);
    }
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05080F), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    ui_status_set_on_home(true);

    // Brand mark (the splash emblem, scaled down and tinted)
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &dacia_logo);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(logo, 92, 26);
    lv_obj_set_style_image_recolor(logo, UI_COLOR_MUTED, 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_pos(logo, LEFT_X, (UI_HEADER_H - 26) / 2);

    // Clock + date (left), alerts panel (right of the clock)
    s_clock = ui_label_create(scr, ui_font_clock, UI_COLOR_TEXT, "--:--");
    lv_obj_set_style_text_letter_space(s_clock, -2, 0);
    lv_obj_set_pos(s_clock, LEFT_X - 4, TOP_Y - 6);
    s_date = ui_label_create(scr, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_set_pos(s_date, LEFT_X, TOP_Y + 92);

    lv_obj_t *alerts = lv_button_create(scr);
    style_pressable(alerts);
    lv_obj_set_style_radius(alerts, 20, 0);
    lv_obj_set_size(alerts, ALERTS_W, ALERTS_H);
    lv_obj_set_pos(alerts, LEFT_X + LEFT_W - ALERTS_W, TOP_Y);
    lv_obj_set_style_pad_all(alerts, 12, 0);
    lv_obj_set_flex_flow(alerts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(alerts, 6, 0);
    lv_obj_add_event_cb(alerts, on_alerts_tap, LV_EVENT_CLICKED, NULL);
    s_alerts = alerts;

    // Live rain map around the car (tap: full radar)
    s_rain = lv_button_create(scr);
    style_pressable(s_rain);
    lv_obj_set_style_pad_all(s_rain, 0, 0);
    lv_obj_set_style_clip_corner(s_rain, true, 0);
    lv_obj_set_size(s_rain, RAIN_NOW_W, RAIN_NOW_H);
    lv_obj_set_pos(s_rain, LEFT_X, 600 - 24 - RAIN_NOW_H);
    lv_obj_add_event_cb(s_rain, on_rain_tap, LV_EVENT_CLICKED, NULL);
    s_rain_canvas = lv_canvas_create(s_rain);
    lv_obj_set_pos(s_rain_canvas, 0, 0);
    lv_obj_set_clickable(s_rain_canvas, false);
    lv_obj_set_hidden(s_rain_canvas, true);
    s_rain_marks = lv_obj_create(s_rain);
    lv_obj_remove_style_all(s_rain_marks);
    lv_obj_set_size(s_rain_marks, RAIN_NOW_W, RAIN_NOW_H);
    lv_obj_set_clickable(s_rain_marks, false);
    s_rain_tag = overlay_label(s_rain, LV_ALIGN_TOP_LEFT, 12, 12);
    s_rain_caption = overlay_label(s_rain, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_max_width(s_rain_caption, RAIN_NOW_W - 24, 0);
    lv_label_set_long_mode(s_rain_caption, LV_LABEL_LONG_DOT);

    // App cards
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, GRID_W, GRID_H);
    lv_obj_set_pos(grid, GRID_X, TOP_Y);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, CARD_GAP, 0);
    lv_obj_set_style_pad_column(grid, CARD_GAP, 0);
    lv_obj_set_scrollable(grid, false);
    for (int i = 0; i < APP_COUNT; i++) {
        card_create(grid, (app_id_t)i);
    }

    refresh_weather();
    fill_alerts();
    fill_rain();
    lv_timer_t *clock = lv_timer_create(clock_update, 1000, NULL);
    ui_screen_own_timer(scr, clock);
    lv_timer_ready(clock);
    weather_add_listener(on_weather, NULL);
    alerts_add_listener(on_alerts, NULL);
    traffic_add_listener(on_alerts, NULL);  // same handler: alerts panel, markers, subtitles
    rain_now_add_listener(on_rain_now, NULL);
    ui_screen_load(scr);
}

// ---------------------------------------------------------------------------
// Boot splash: animated Dacia emblem while Wi-Fi joins the saved network in the background.

#define SPLASH_MIN_MS   2400                     // let the animation finish even on fast connects
#define LOGO_COLOR      lv_color_hex(0xE8EEF2)
#define DRL_COLOR       lv_color_hex(0x5EEAD4)   // "daytime running light" bar under the emblem

extern const lv_image_dsc_t dacia_logo;

static lv_obj_t *s_splash;
static lv_obj_t *s_splash_status;
static lv_timer_t *s_splash_timer;
static bool s_splash_min_done;
static bool s_splash_connected;

static void splash_continue(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (!settings_get_wifi(ssid, pass)) {
        ui_wifi_open(false);
    } else if (s_splash_connected || wifi_mgr_state() == WIFI_MGR_CONNECTED) {
        ui_home_open();
    } else {
        lv_obj_set_hidden(s_splash_status, false);  // still joining: show progress + escape hatch
    }
}

static void on_splash_wifi(wifi_mgr_state_t st, const char *reason, void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_splash) {
        if (st == WIFI_MGR_CONNECTED) {
            s_splash_connected = true;
            if (s_splash_min_done) {
                ui_home_open();
            }
        } else if (st == WIFI_MGR_FAILED) {
            char buf[96];
            if (reason && strcmp(reason, "Wrong password") == 0) {
                // Retrying won't help: ask for the password again.
                wifi_mgr_disconnect();
                snprintf(buf, sizeof(buf), "Couldn't join %s: wrong password", wifi_mgr_ssid());
                ui_wifi_open_with_error(buf);
            } else {
                // Probably out of range / hotspot not up yet: keep retrying in the background.
                snprintf(buf, sizeof(buf), "%s not available, retrying...", wifi_mgr_ssid());
                ui_home_open();
                ui_toast(buf);
            }
        }
    }
    ui_unlock();
}

static void on_splash_timer(lv_timer_t *t)
{
    s_splash_timer = NULL;  // one-shot: LVGL deletes it after this call
    s_splash_min_done = true;
    if (s_splash) {
        splash_continue();
    }
}

static void on_splash_setup(lv_event_t *e)
{
    wifi_mgr_disconnect();
    ui_wifi_open(false);
}

static void on_splash_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_splash) {
        return;  // a newer instance of this screen already took over the shared state
    }
    wifi_mgr_remove_listener(on_splash_wifi, NULL);
    if (s_splash_timer) {
        lv_timer_delete(s_splash_timer);
        s_splash_timer = NULL;
    }
    s_splash = NULL;
}

static void anim_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
}

static void anim_scale(void *obj, int32_t v)
{
    lv_image_set_scale(obj, v);
}

static void anim_width(void *obj, int32_t v)
{
    lv_obj_set_width(obj, v);
}

static void animate(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                    uint32_t delay, uint32_t time, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, time);
    lv_anim_set_path_cb(&a, path);
    cb(obj, from);
    lv_anim_start(&a);
}

void ui_splash_open(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    bool have_creds = settings_get_wifi(ssid, pass);

    lv_obj_t *scr = ui_screen_create();
    s_splash = scr;
    s_splash_min_done = false;
    s_splash_connected = false;
    lv_obj_add_event_cb(scr, on_splash_delete, LV_EVENT_DELETE, NULL);
    // Subtle vertical gradient towards black, like a dark dashboard at night.
    lv_obj_set_style_bg_grad_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &dacia_logo);
    lv_obj_set_style_image_recolor(logo, LOGO_COLOR, 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_image_set_pivot(logo, dacia_logo.header.w / 2, dacia_logo.header.h / 2);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *drl = lv_obj_create(scr);
    lv_obj_remove_style_all(drl);
    lv_obj_set_size(drl, 0, 4);
    lv_obj_set_style_radius(drl, 2, 0);
    lv_obj_set_style_bg_color(drl, DRL_COLOR, 0);
    lv_obj_set_style_bg_opa(drl, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(drl, DRL_COLOR, 0);
    lv_obj_set_style_shadow_width(drl, 24, 0);
    lv_obj_set_style_shadow_opa(drl, LV_OPA_70, 0);
    lv_obj_align(drl, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *title = ui_label_create(scr, ui_font_lg, UI_COLOR_MUTED, "DashCar");
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 90);

    // Status row, revealed only if Wi-Fi is still joining once the animation is over.
    s_splash_status = lv_obj_create(scr);
    lv_obj_remove_style_all(s_splash_status);
    lv_obj_set_size(s_splash_status, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(s_splash_status, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(s_splash_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_splash_status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_splash_status, 16, 0);
    lv_obj_t *sp = lv_spinner_create(s_splash_status);
    lv_obj_set_size(sp, 36, 36);
    char buf[64];
    snprintf(buf, sizeof(buf), "Connecting to %s...", ssid);
    ui_label_create(s_splash_status, ui_font_md, UI_COLOR_MUTED, buf);
    ui_button_create(s_splash_status, LV_SYMBOL_SETTINGS "  Wi-Fi setup", UI_COLOR_CARD_HI, on_splash_setup, NULL);
    lv_obj_set_hidden(s_splash_status, true);

    // Emblem fades and grows in, then the light bar sweeps open, then the title appears.
    animate(logo, anim_opa, 0, 255, 100, 900, lv_anim_path_ease_out);
    animate(logo, anim_scale, 200, 256, 100, 1100, lv_anim_path_overshoot);
    animate(drl, anim_width, 0, 560, 900, 700, lv_anim_path_ease_in_out);
    animate(title, anim_opa, 0, 255, 1400, 600, lv_anim_path_linear);

    s_splash_timer = lv_timer_create(on_splash_timer, SPLASH_MIN_MS, NULL);
    lv_timer_set_repeat_count(s_splash_timer, 1);

    if (have_creds) {
        wifi_mgr_add_listener(on_splash_wifi, NULL);
        wifi_mgr_connect(ssid, pass, true);
    }
    ui_screen_load(scr);
}
