// Home: glance area (clock, date, weather now, rain soon) + live app cards.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "net/wifi_mgr.h"
#include "services/alerts.h"
#include "services/cameras.h"
#include "services/fuel.h"   // fuel_distance_km
#include "services/gps.h"
#include "services/location.h"
#include "services/map_tiles.h"
#include "services/parking.h"
#include "services/rain_now.h"
#include "services/speedlimit.h"
#include "services/trip.h"
#include "services/traffic.h"
#include "services/weather.h"
#include "settings.h"
#include "esp_attr.h"
#include "ui.h"
#include "wx_icon.h"

extern const lv_image_dsc_t app_logo_small;

#define LEFT_X    32
#define LEFT_W    440
#define GRID_X    (LEFT_X + LEFT_W + 24)
#define GRID_W    (1024 - GRID_X - 24)
#define TOP_Y     UI_HEADER_H
#define GRID_H    (600 - TOP_Y - 24)
#define ALERTS_W  226   // panel right of the clock
#define ALERTS_H  156
#define RAIN_YOU_SIZE 34   // car arrow on the rain map (the stopped dot is half that)
#define GRID_COLS 3   // 3 columns; 2 rows fit, the grid scrolls to the rest
#define CARD_GAP  12
#define CARD_W    ((GRID_W - (GRID_COLS - 1) * CARD_GAP) / GRID_COLS)
#define CARD_H    ((GRID_H - CARD_GAP) / 2)

#define C_SURFACE lv_color_hex(0x111827)
#define C_EDGE    lv_color_hex(0x1F2A3D)

// Order = grid order: two rows show, the rest is a scroll away.
typedef enum {
    APP_WEATHER, APP_RADAR, APP_TRAFFIC,
    APP_FUEL, APP_GPS, APP_TRIP,
    APP_EXPLORE, APP_CAMERAS, APP_PARKING,
    APP_SETTINGS,
    APP_COUNT
} app_id_t;

#define GRID_ROWS  ((APP_COUNT + GRID_COLS - 1) / GRID_COLS)
#define GRID_PAGES (GRID_ROWS - 1)   // scroll stops: rows 0..GRID_ROWS-2 at the top (two rows visible)

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
    [APP_GPS]     = { "GPS",     0x38BDF8, ui_gps_open },
    [APP_TRIP]    = { "Trip",    0xF472B6, ui_trip_open },
    [APP_CAMERAS] = { "Cameras", 0xFB923C, ui_cameras_open },
    [APP_EXPLORE] = { "Explore", 0x84CC16, ui_explore_open },
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
static lv_obj_t *s_rain_you;        // car marker on it (arrow while driving)
static lv_obj_t *s_rain_tag;        // "RAIN RADAR · 18:00"
static lv_obj_t *s_rain_caption;    // "No rain expected in the next 2 hours"
static lv_obj_t *s_badge[APP_COUNT];
static lv_obj_t *s_sub[APP_COUNT];  // live subtitles
static lv_obj_t *s_fuel_qr;         // QR to the nearest station (replaces the Fuel badge)
static lv_obj_t *s_dots[GRID_PAGES]; // page indicator under the scrolling card grid
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

static void icon_gps(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Crosshair: ring, centre dot and four ticks.
    ring(box, s / 2, s / 2, s * 70 / 100, 3, c, LV_OPA_COVER);
    shape(box, s / 2 - s / 10, s / 2 - s / 10, s / 5, s / 5, LV_RADIUS_CIRCLE, c);
    const int32_t t = s * 22 / 100, w = 3;
    shape(box, s / 2 - w / 2, 0, w, t, 1, c);
    shape(box, s / 2 - w / 2, s - t, w, t, 1, c);
    shape(box, 0, s / 2 - w / 2, t, w, 1, c);
    shape(box, s - t, s / 2 - w / 2, t, w, 1, c);
}

static void icon_trip(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // A route: start ring, dotted path, end pin.
    ring(box, s * 18 / 100, s * 80 / 100, s * 30 / 100, 3, c, LV_OPA_COVER);
    for (int i = 0; i < 3; i++) {
        shape(box, s * (34 + i * 14) / 100, s * (62 - i * 14) / 100, s / 9, s / 9, LV_RADIUS_CIRCLE, c);
    }
    shape(box, s * 64 / 100, s * 4 / 100, s * 32 / 100, s * 32 / 100, LV_RADIUS_CIRCLE, c);
    shape(box, s * 75 / 100, s * 28 / 100, s * 10 / 100, s * 16 / 100, 2, c);
}

static void icon_camera(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Speed camera box on a post, lens on the front.
    shape(box, s * 10 / 100, s * 18 / 100, s * 70 / 100, s * 40 / 100, s / 10, c);
    ring(box, s * 30 / 100, s * 38 / 100, s * 26 / 100, 3, C_SURFACE, LV_OPA_COVER);
    shape(box, s * 80 / 100, s * 26 / 100, s * 14 / 100, s * 10 / 100, 2, c);   // flash
    shape(box, s * 42 / 100, s * 58 / 100, s * 10 / 100, s * 38 / 100, 2, c);   // post
}

static void icon_explore(lv_obj_t *box, int32_t s, lv_color_t c)
{
    // Tent under a mountain ridge: two triangles drawn as rotated bars would be fussy, so a peak of
    // three stacked bars and a tent door.
    for (int i = 0; i < 4; i++) {
        const int32_t w = s * (22 + i * 22) / 100;
        shape(box, (s - w) / 2, s * (14 + i * 18) / 100, w, s * 16 / 100, 2, c);
    }
    shape(box, s / 2 - s / 12, s * 66 / 100, s / 6, s * 22 / 100, 1, C_SURFACE);
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
    case APP_GPS:
        icon_gps(box, s, c);
        break;
    case APP_TRIP:
        icon_trip(box, s, c);
        break;
    case APP_CAMERAS:
        icon_camera(box, s, c);
        break;
    case APP_EXPLORE:
        icon_explore(box, s, c);
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

// Distance to the parked car, from the GPS only (an IP/city location would be meaningless at this
// scale); -1 if unknown. Updated every LOCATION_FOLLOW_MS by on_follow.
static float s_park_km = -1;

static void update_park_distance(void)
{
    parking_spot_t p;
    parking_get(&p);
    location_t loc;
    s_park_km = !p.demo && location_get(&loc) && loc.source == LOCATION_GPS ?
                fuel_distance_km(loc.lat, loc.lon, (float)p.lat, (float)p.lon) : -1;
}

// Parked card subtitle: age of the spot, and how far it is when the GPS knows where we are.
static void update_parking_card(void)
{
    parking_spot_t p;
    parking_get(&p);
    char buf[48];
    if (p.demo) {
        ui_label_set_text_if_changed(s_sub[APP_PARKING], "Demo spot\nnone saved yet");
    } else if (s_park_km >= 0) {
        char age[32];
        ui_parking_age(&p, age, sizeof(age));
        char *sep = strstr(age, "  ·  ");
        if (sep) {
            *sep = '\0';  // "2 h ago"
        }
        if (s_park_km < 1) {
            snprintf(buf, sizeof(buf), "%s\n%d m away", age, (int)(s_park_km * 1000 / 10) * 10);
        } else {
            snprintf(buf, sizeof(buf), "%s\n%.1f km away", age, s_park_km);
        }
        ui_label_set_text_if_changed(s_sub[APP_PARKING], buf);
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

// GPS card: "9 satellites" / "87 km/h", or the search state.
static void update_gps_card(void)
{
    gps_fix_t f;
    gps_get(&f);
    int sats;
    char buf[64];
    switch (gps_state(&sats)) {
    case GPS_FIX: {
        const bool imperial = settings_get_imperial();
        const float v = imperial ? f.speed_kmh / 1.609344f : f.speed_kmh;
        char s[24] = "Fix";
        speedlimit_t sl;
        speedlimit_get(&sl);
        if (sl.kmh > 0) {
            snprintf(s, sizeof(s), "Limit %d", sl.kmh);
        } else if (sats > 0) {
            snprintf(s, sizeof(s), "%d satellites", sats);
        }
        snprintf(buf, sizeof(buf), "%s\n%ld %s", s, v < 2 ? 0L : lroundf(v), imperial ? "mph" : "km/h");
        break;
    }
    case GPS_SEARCHING:
        if (sats >= 0) {
            snprintf(buf, sizeof(buf), "Searching\n%d in view", sats);
        } else {
            snprintf(buf, sizeof(buf), "Searching...");
        }
        break;
    default:
        snprintf(buf, sizeof(buf), "No GPS data");
        break;
    }
    ui_label_set_text_if_changed(s_sub[APP_GPS], buf);
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
    update_gps_card();
    // Trip: distance + driving time of the current trip
    trip_info_t trip;
    trip_get(&trip);
    if (trip.current.start) {
        const bool imp = settings_get_imperial();
        snprintf(buf, sizeof(buf), "%.1f %s\n%lu:%02lu driving", imp ? trip.current.km / 1.609344f : trip.current.km,
                 imp ? "mi" : "km", (unsigned long)(trip.current.moving_s / 3600),
                 (unsigned long)(trip.current.moving_s / 60 % 60));
    } else {
        snprintf(buf, sizeof(buf), "No trip yet\ndrive to start");
    }
    ui_label_set_text_if_changed(s_sub[APP_TRIP], buf);
    // Cameras: alert state + data
    const camera_t *cams;
    const int ncam = cameras_list(&cams);
    if (ncam == 0) {
        snprintf(buf, sizeof(buf), cameras_loading() ? "Downloading..." : "Alerts %s\nno data yet",
                 ui_cam_alert_enabled() ? "on" : "off");
    } else {
        snprintf(buf, sizeof(buf), "Alerts %s\n%d in Spain", ui_cam_alert_enabled() ? "on" : "off", ncam);
    }
    ui_label_set_text_if_changed(s_sub[APP_CAMERAS], buf);
    // Explore runs only when open: a fixed subtitle, no live data.
    ui_label_set_text_if_changed(s_sub[APP_EXPLORE], "Water, camps,\nhuts, viewpoints");
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

// You: the map is recentred only every 10 km (services/rain_now), so the marker is placed from
// the current position, and points along the GPS course while driving (clock_update, 1 s).
static void rain_you_update(void)
{
    if (!s_rain_you) {
        return;
    }
    int32_t ox, oy;
    rain_now_origin(&ox, &oy);
    location_t loc;
    int32_t yx = RAIN_NOW_W / 2, yy = RAIN_NOW_H / 2;
    if (location_get(&loc)) {
        const int32_t m = RAIN_YOU_SIZE / 2;
        yx = LV_CLAMP(m, (int32_t)lround(mt_lon_to_px(loc.lon, RAIN_NOW_ZOOM)) - ox, RAIN_NOW_W - m);
        yy = LV_CLAMP(m, (int32_t)lround(mt_lat_to_px(loc.lat, RAIN_NOW_ZOOM)) - oy, RAIN_NOW_H - m);
    }
    yx -= RAIN_YOU_SIZE / 2;
    yy -= RAIN_YOU_SIZE / 2;
    if (lv_obj_get_x(s_rain_you) != yx || lv_obj_get_y(s_rain_you) != yy) {
        lv_obj_set_pos(s_rain_you, yx, yy);
    }
    ui_heading_marker_set(s_rain_you, ui_gps_heading());
}

static void fill_rain(void)
{
    time_t radar_t = 0;
    const uint16_t *img = rain_now_image(&radar_t);
    lv_obj_clean(s_rain_marks);
    s_rain_you = NULL;
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
    s_rain_you = ui_heading_marker_create(s_rain_marks, RAIN_YOU_SIZE, UI_COLOR_ACCENT);
    rain_you_update();

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
    rain_you_update();
}

// GPS-following refresh of what depends on the position but needs no network.
static void on_follow(lv_timer_t *t)
{
    fill_rain();  // "you" dot and incident markers
    update_park_distance();
    update_subtitles();
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
    s_rain_you = NULL;
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

static lv_obj_t *card_create(lv_obj_t *grid, app_id_t id)
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
    return card;
}

static void on_grid_scroll(lv_event_t *e)
{
    lv_obj_t *grid = lv_event_get_target_obj(e);
    const int page = (lv_obj_get_scroll_y(grid) + (CARD_H + CARD_GAP) / 2) / (CARD_H + CARD_GAP);
    for (int i = 0; i < GRID_PAGES; i++) {
        const lv_opa_t opa = i == page ? LV_OPA_COVER : LV_OPA_30;
        if (lv_obj_get_style_bg_opa(s_dots[i], 0) != opa) {
            lv_obj_set_style_bg_opa(s_dots[i], opa, 0);
        }
    }
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

    // Brand mark: the gauge emblem + name, tinted
    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &app_logo_small);
    lv_obj_set_style_image_recolor(logo, UI_COLOR_MUTED, 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_pos(logo, LEFT_X, (UI_HEADER_H - app_logo_small.header.h) / 2);
    lv_obj_t *brand = ui_label_create(scr, ui_font_sm, UI_COLOR_MUTED, "DashCar");
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align_to(brand, logo, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

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
    // More cards than fit: scroll up for the next row (snaps to rows; no scrollbar, dots below).
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(grid, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(grid, false);
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *card = card_create(grid, (app_id_t)i);
        // Snap points: the first two rows only (row 2 at the top = fully scrolled). Snapping the
        // last row to the top would scroll past the end and leave the grid half empty.
        lv_obj_set_snappable(card, i % GRID_COLS == 0 && i / GRID_COLS < GRID_PAGES);
    }
    lv_obj_add_event_cb(grid, on_grid_scroll, LV_EVENT_SCROLL, NULL);
    for (int i = 0; i < GRID_PAGES; i++) {
        s_dots[i] = shape(scr, GRID_X + GRID_W / 2 - GRID_PAGES * 10 + 6 + i * 20, 600 - 15, 8, 8, LV_RADIUS_CIRCLE,
                          UI_COLOR_MUTED);
        lv_obj_set_style_bg_opa(s_dots[i], i == 0 ? LV_OPA_COVER : LV_OPA_30, 0);
    }

    refresh_weather();
    fill_alerts();
    fill_rain();
    update_park_distance();
    lv_timer_t *clock = lv_timer_create(clock_update, 1000, NULL);
    ui_screen_own_timer(scr, clock);
    ui_screen_own_timer(scr, lv_timer_create(on_follow, LOCATION_FOLLOW_MS, NULL));
    lv_timer_ready(clock);
    weather_add_listener(on_weather, NULL);
    alerts_add_listener(on_alerts, NULL);
    traffic_add_listener(on_alerts, NULL);  // same handler: alerts panel, markers, subtitles
    rain_now_add_listener(on_rain_now, NULL);
    ui_screen_load(scr);
}

// ---------------------------------------------------------------------------
// Boot splash on a near-black background, while Wi-Fi joins the saved network in the background.
// Two compositions (main/CMakeLists.txt picks the emblem file, tools/gen_logo.py makes both):
//  - generic (ui/emblem.c, the DashCar "DC" monogram): an instrument-cluster ignition sequence. The
//    tachometer's ticks light up, the needle sweeps to the red line and back, the monogram dashes in
//    and gets a chrome shine, "DASHCAR" fades in below.
//  - owner's build (ui/emblem_private.c, DASHCAR_EMBLEM_PRIVATE): the emblem's two halves slide in and
//    lock together with a flash, a shine sweeps over it and a light bar opens under it.

#define SPLASH_MIN_MS   2600                     // let the animation finish even on fast connects
#define EMBLEM_COLOR    lv_color_hex(0xB9C3CE)   // brushed metal; the shine sweeps white over it
#define SHINE_W         120                      // widest of the three shine bands
#if DASHCAR_EMBLEM_PRIVATE
#define DRL_COLOR       lv_color_hex(0x5EEAD4)   // "daytime running light" bar under the emblem
#define EMBLEM_W        600                      // emblem width, px
#define EMBLEM_CY       280                      // emblem centre, screen y (a little above the middle)
#define EMBLEM_SLIDE    240                      // each half starts this far out
#define SNAP_AT         200                      // ms: halves start sliding in
#define SNAP_MS         600                      // ...and lock together at SNAP_AT + SNAP_MS
#else
#define EMBLEM_W        290                      // monogram width, px (clear of the dial numbers)
#define DIAL_CX         512
#define DIAL_CY         290
#define DIAL_R          238                      // outer arc radius
#define DIAL_C          (DIAL_R + 12)            // centre inside the dial box (room for the needle tip)
#define DIAL_TICKS      33                       // 8 majors (0..8, x1000 rpm) with 3 minors between
#define DIAL_RED        27                       // first red-line tick (6.75)
#define DIAL_START      135                      // degrees, LVGL style (clockwise from 3 o'clock)
#define DIAL_SPAN       270
#define TICK_DIM        lv_color_hex(0x263247)
#define TICK_LIT        lv_color_hex(0xCBD5E1)
#define TICK_RED        lv_color_hex(0xEF4444)
#define SWEEP_COLOR     lv_color_hex(0x5EEAD4)
#define NEEDLE_COLOR    lv_color_hex(0xFB923C)
#endif
extern const float app_emblem_pts[][2];          // emblem outlines (tools/gen_logo.py)
extern const float app_emblem_w, app_emblem_h;
extern const int app_emblem_contours;
extern const int app_emblem_contour_end[];

// Not in LVGL 9.6's public headers (src/misc/cache/instance/lv_image_cache.h), but exported. Needed
// before freeing an image's pixels: the image cache keys entries by the descriptor's address.
void lv_image_cache_drop(const void *src);

static lv_image_dsc_t s_emblem_dsc;  // rasterised emblem (A8, PSRAM), freed with the splash
static uint8_t *s_emblem_px;

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
    if (s_emblem_px) {
        // The emblem images are deleted with the screen right after this, without another render,
        // so the buffer can go now.
        lv_image_cache_drop(&s_emblem_dsc);
        heap_caps_free(s_emblem_px);
        s_emblem_px = NULL;
    }
    if (s_splash_timer) {
        lv_timer_delete(s_splash_timer);
        s_splash_timer = NULL;
    }
    s_splash = NULL;
}

// Emblem outlines -> A8 coverage: even-odd scanline fill with 4 sub-rows per pixel and exact
// horizontal coverage, so the edges are anti-aliased. ~100 KB, once per boot, a few ms.
static bool emblem_rasterise(void)
{
    const float scale = EMBLEM_W / app_emblem_w;
    const int w = EMBLEM_W, h = (int)lroundf(app_emblem_h * scale);
    s_emblem_px = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    float *cov = heap_caps_malloc((w + 1) * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_emblem_px || !cov) {
        heap_caps_free(s_emblem_px);
        heap_caps_free(cov);
        s_emblem_px = NULL;
        return false;
    }
    for (int y = 0; y < h; y++) {
        memset(cov, 0, (w + 1) * sizeof(float));
        for (int sub = 0; sub < 4; sub++) {
            const float sy = (y + (sub + 0.5f) / 4) / scale;  // outline units
            float xs[16];
            int nx = 0;
            for (int c = 0, first = 0; c < app_emblem_contours; first = app_emblem_contour_end[c++]) {
                const int last = app_emblem_contour_end[c] - 1;
                for (int i = first; i <= last && nx < 16; i++) {
                    const float *a = app_emblem_pts[i], *b = app_emblem_pts[i == last ? first : i + 1];
                    if ((a[1] <= sy) != (b[1] <= sy)) {
                        xs[nx++] = (a[0] + (sy - a[1]) * (b[0] - a[0]) / (b[1] - a[1])) * scale;
                    }
                }
            }
            for (int i = 1; i < nx; i++) {  // insertion sort, a handful of crossings
                for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
                    const float t = xs[j];
                    xs[j] = xs[j - 1];
                    xs[j - 1] = t;
                }
            }
            for (int k = 0; k + 1 < nx; k += 2) {
                const float x0 = LV_CLAMP(0, xs[k], w), x1 = LV_CLAMP(0, xs[k + 1], w);
                const int i0 = (int)x0, i1 = (int)x1;
                if (i0 == i1) {
                    cov[i0] += x1 - x0;
                    continue;
                }
                cov[i0] += i0 + 1 - x0;
                for (int i = i0 + 1; i < i1; i++) {
                    cov[i] += 1;
                }
                cov[i1] += x1 - i1;
            }
        }
        for (int x = 0; x < w; x++) {
            s_emblem_px[y * w + x] = (uint8_t)LV_MIN(255, (int)lroundf(cov[x] * (255.0f / 4)));
        }
    }
    heap_caps_free(cov);
    s_emblem_dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = w, .h = h, .stride = w },
        .data_size = (uint32_t)(w * h),
        .data = s_emblem_px,
    };
    return true;
}

// A clipping window `w` px wide onto the emblem, showing it from emblem x `sx` (the halves and the
// shine bands are all windows onto the same image).
static lv_obj_t *emblem_window(lv_obj_t *parent, int32_t sx, int32_t w, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, s_emblem_dsc.header.h);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);
    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, &s_emblem_dsc);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, -sx, 0);
    return box;
}

static void anim_x(void *obj, int32_t v)
{
    lv_obj_set_x(obj, v);
}

// Image opacity of a window's emblem (no layer, unlike the object's own opa with children).
static void anim_window_opa(void *obj, int32_t v)
{
    lv_obj_set_style_image_opa(lv_obj_get_child(obj, 0), v, 0);
}

static int32_t s_emblem_x;         // screen x of the assembled emblem
static lv_obj_t *s_shine[3];

// Shine: three nested bands (wide and faint .. narrow and bright) centred on v.
static void shine_move(lv_anim_t *a, int32_t v)
{
    (void)a;
    for (int i = 0; i < 3; i++) {
        const int32_t x = v - lv_obj_get_width(s_shine[i]) / 2;
        lv_obj_set_x(s_shine[i], x);
        lv_obj_set_x(lv_obj_get_child(s_shine[i], 0), s_emblem_x - x);
    }
}

#if DASHCAR_EMBLEM_PRIVATE
static lv_obj_t *s_flare, *s_flash;

// The lock flash: a horizontal flare opening out of the junction and a glow, quick in, slow out.
static void flash_step(lv_anim_t *a, int32_t v)
{
    (void)a;
    const int32_t opa = v < 120 ? v * 255 / 120 : (1000 - v) * 255 / 880;
    const int32_t w = 40 + (int32_t)(860 * (1 - expf(-v / 180.0f)));
    lv_obj_set_width(s_flare, w);
    lv_obj_set_x(s_flare, 512 - w / 2);
    lv_obj_set_style_bg_opa(s_flare, opa, 0);
    lv_obj_set_style_shadow_opa(s_flare, opa, 0);
    lv_obj_set_style_bg_opa(s_flash, opa, 0);
    lv_obj_set_style_shadow_opa(s_flash, opa, 0);
}

static lv_obj_t *glow(lv_obj_t *parent, int32_t w, int32_t h, int32_t shadow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(0xDDE7F0), 0);
    lv_obj_set_style_shadow_width(o, shadow, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

#endif

static void custom_anim(lv_obj_t *var, lv_anim_custom_exec_cb_t cb, int32_t from, int32_t to,
                        uint32_t delay, uint32_t time, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_custom_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, time);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_early_apply(&a, false);  // LVGL 9 applies `from` at start by default, even with a delay
    lv_anim_start(&a);
}

#if DASHCAR_EMBLEM_PRIVATE
static void anim_width(void *obj, int32_t v)
{
    lv_obj_set_width(obj, v);
}
#endif

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

#if !DASHCAR_EMBLEM_PRIVATE
static void anim_label_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
}
#endif

// Three nested shine bands over the emblem at screen y `ey`, parked left of it.
static void shine_create(lv_obj_t *scr, int32_t ey)
{
    static const int32_t band_w[3] = { SHINE_W, SHINE_W / 2, SHINE_W / 5 };
    static const lv_opa_t band_opa[3] = { 70, 110, 255 };
    for (int i = 0; i < 3; i++) {
        s_shine[i] = emblem_window(scr, 0, band_w[i], lv_color_white());
        lv_obj_set_y(s_shine[i], ey);
        lv_obj_set_style_image_opa(lv_obj_get_child(s_shine[i], 0), band_opa[i], 0);
    }
    shine_move(NULL, s_emblem_x - SHINE_W);
}

#if DASHCAR_EMBLEM_PRIVATE
// Owner's build: the emblem's halves slide in and lock with a flash, shine, light bar.
static void splash_emblem_lock(lv_obj_t *scr)
{
    // Emblem: the two halves (split at the pinch in the middle), shine bands over them, flash.
    lv_obj_t *half[2] = { NULL, NULL };
    if (s_emblem_px || emblem_rasterise()) {
        const int32_t eh = s_emblem_dsc.header.h, split = EMBLEM_W / 2, ey = EMBLEM_CY - eh / 2;
        s_emblem_x = (1024 - EMBLEM_W) / 2;
        half[0] = emblem_window(scr, 0, split, EMBLEM_COLOR);
        half[1] = emblem_window(scr, split, EMBLEM_W - split, EMBLEM_COLOR);
        lv_obj_set_y(half[0], ey);
        lv_obj_set_y(half[1], ey);
        shine_create(scr, ey);
        s_flare = glow(scr, 40, 4, 30);
        lv_obj_set_y(s_flare, EMBLEM_CY - 2);
        s_flash = glow(scr, 30, 30, 90);
        lv_obj_set_pos(s_flash, 512 - 15, EMBLEM_CY - 15);
    }

    lv_obj_t *drl = lv_obj_create(scr);
    lv_obj_remove_style_all(drl);
    lv_obj_set_size(drl, 0, 4);
    lv_obj_set_style_radius(drl, 2, 0);
    lv_obj_set_style_bg_color(drl, DRL_COLOR, 0);
    lv_obj_set_style_bg_opa(drl, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(drl, DRL_COLOR, 0);
    lv_obj_set_style_shadow_width(drl, 24, 0);
    lv_obj_set_style_shadow_opa(drl, LV_OPA_70, 0);
    lv_obj_align(drl, LV_ALIGN_TOP_MID, 0, EMBLEM_CY + (int32_t)(app_emblem_h * EMBLEM_W / app_emblem_w) / 2 + 48);

    // The halves fade in while sliding together, accelerating like magnets, and lock with a flash;
    // a shine sweeps over the emblem and the light bar opens under it.
    if (half[0]) {
        for (int i = 0; i < 2; i++) {
            const int32_t x = s_emblem_x + (i ? EMBLEM_W / 2 : 0);
            animate(half[i], anim_x, x + (i ? EMBLEM_SLIDE : -EMBLEM_SLIDE), x, SNAP_AT, SNAP_MS, lv_anim_path_ease_in);
            animate(half[i], anim_window_opa, 0, 255, SNAP_AT, SNAP_MS * 2 / 3, lv_anim_path_linear);
        }
        custom_anim(s_flash, flash_step, 0, 1000, SNAP_AT + SNAP_MS, 650, lv_anim_path_linear);
        custom_anim(s_shine[0], shine_move, s_emblem_x - SHINE_W, s_emblem_x + EMBLEM_W + SHINE_W,
                    SNAP_AT + SNAP_MS + 250, 700, lv_anim_path_ease_in_out);
    }
    animate(drl, anim_width, 0, EMBLEM_W - 40, SNAP_AT + SNAP_MS + 400, 700, lv_anim_path_ease_in_out);

}
#else
// Generic: tachometer ignition sequence around the DashCar monogram.
static lv_obj_t *s_tick[DIAL_TICKS];
static lv_obj_t *s_tick_label[DIAL_TICKS / 4 + 1];
static uint8_t s_tick_state[DIAL_TICKS];   // 0 dim, 1 lit, 2 under the sweep
static lv_obj_t *s_dial_arc, *s_needle;

static float dial_angle(float frac)        // radians, LVGL orientation (y down)
{
    return (DIAL_START + DIAL_SPAN * frac) * (float)M_PI / 180.0f;
}

static void tick_set(int i, uint8_t state)
{
    if (s_tick_state[i] == state) {
        return;  // style changes redraw: only on a real change
    }
    s_tick_state[i] = state;
    lv_color_t c = state == 0 ? TICK_DIM : i >= DIAL_RED ? TICK_RED : state == 2 ? SWEEP_COLOR : TICK_LIT;
    lv_obj_set_style_line_color(s_tick[i], c, 0);
    lv_obj_set_style_line_opa(s_tick[i], state == 0 ? LV_OPA_60 : LV_OPA_COVER, 0);
    if (i % 4 == 0) {
        lv_obj_set_style_opa(s_tick_label[i / 4], state ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

// Ticks light up one after another around the dial; v = number lit.
static void dial_light(lv_anim_t *a, int32_t v)
{
    (void)a;
    for (int i = 0; i < DIAL_TICKS; i++) {
        tick_set(i, i < v ? 1 : 0);
    }
}

// Needle position v = 0..1000 (0 rpm .. the end of the scale): arc fill, needle, ticks under it.
static void dial_sweep(lv_anim_t *a, int32_t v)
{
    (void)a;
    lv_arc_set_value(s_dial_arc, v);
    const float ang = dial_angle(v / 1000.0f);
    static lv_point_precise_t pts[2];  // lv_line keeps the pointer
    pts[0] = (lv_point_precise_t){ DIAL_C + (DIAL_R - 56) * cosf(ang), DIAL_C + (DIAL_R - 56) * sinf(ang) };
    pts[1] = (lv_point_precise_t){ DIAL_C + (DIAL_R + 6) * cosf(ang), DIAL_C + (DIAL_R + 6) * sinf(ang) };
    lv_line_set_points(s_needle, pts, 2);
    const int under = (int)lroundf(v / 1000.0f * (DIAL_TICKS - 1));
    for (int i = 0; i < DIAL_TICKS; i++) {
        tick_set(i, i <= under && v > 0 ? 2 : 1);
    }
}

static void splash_ignition(lv_obj_t *scr)
{
    // Everything dial-related lives in a box of the dial's size, so its points are local.
    lv_obj_t *dial = lv_obj_create(scr);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, 2 * DIAL_C, 2 * DIAL_C);
    lv_obj_set_pos(dial, DIAL_CX - DIAL_C, DIAL_CY - DIAL_C);
    lv_obj_set_clickable(dial, false);
    lv_obj_set_scrollable(dial, false);

    // Track + sweep: an arc with the needle's value; the track itself draws in first.
    s_dial_arc = lv_arc_create(dial);
    lv_obj_set_size(s_dial_arc, 2 * DIAL_R, 2 * DIAL_R);
    lv_obj_center(s_dial_arc);
    lv_obj_remove_style(s_dial_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_dial_arc, false);
    lv_arc_set_rotation(s_dial_arc, DIAL_START);
    lv_arc_set_bg_angles(s_dial_arc, 0, DIAL_SPAN);
    lv_arc_set_range(s_dial_arc, 0, 1000);
    lv_arc_set_value(s_dial_arc, 0);
    lv_obj_set_style_arc_width(s_dial_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_dial_arc, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_dial_arc, SWEEP_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_dial_arc, LV_OPA_60, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_dial_arc, 0, 0);

    // Ticks (majors longer, with the numbers inside), dim until lit.
    static lv_point_precise_t tick_pts[DIAL_TICKS][2];
    for (int i = 0; i < DIAL_TICKS; i++) {
        const bool major = i % 4 == 0;
        const float ang = dial_angle((float)i / (DIAL_TICKS - 1));
        const float r0 = DIAL_R - 16, r1 = DIAL_R - (major ? 42 : 28);
        tick_pts[i][0] = (lv_point_precise_t){ DIAL_C + r0 * cosf(ang), DIAL_C + r0 * sinf(ang) };
        tick_pts[i][1] = (lv_point_precise_t){ DIAL_C + r1 * cosf(ang), DIAL_C + r1 * sinf(ang) };
        s_tick[i] = lv_line_create(dial);
        lv_line_set_points(s_tick[i], tick_pts[i], 2);
        lv_obj_set_style_line_width(s_tick[i], major ? 5 : 3, 0);
        lv_obj_set_style_line_rounded(s_tick[i], true, 0);
        lv_obj_set_clickable(s_tick[i], false);
        if (major) {
            char num[4];
            snprintf(num, sizeof(num), "%d", i / 4);
            lv_obj_t *l = ui_label_create(dial, ui_font_md, i >= DIAL_RED ? TICK_RED : UI_COLOR_MUTED, num);
            const float rl = DIAL_R - 66;
            lv_obj_align(l, LV_ALIGN_CENTER, (int32_t)lroundf(rl * cosf(ang)), (int32_t)lroundf(rl * sinf(ang)));
            s_tick_label[i / 4] = l;
        }
        s_tick_state[i] = 0xFF;
        tick_set(i, 0);
    }

    // Needle: a bright bar across the arc (the centre stays free for the monogram).
    s_needle = lv_line_create(dial);
    lv_obj_set_style_line_width(s_needle, 6, 0);
    lv_obj_set_style_line_rounded(s_needle, true, 0);
    lv_obj_set_style_line_color(s_needle, NEEDLE_COLOR, 0);
    lv_obj_set_clickable(s_needle, false);
    dial_sweep(NULL, 0);
    for (int i = 0; i < DIAL_TICKS; i++) {
        tick_set(i, 0);
    }

    lv_obj_t *rpm = ui_label_create(dial, ui_font_sm, UI_COLOR_MUTED, "x1000 rpm");
    lv_obj_align(rpm, LV_ALIGN_CENTER, 0, -DIAL_R / 2 - 4);
    lv_obj_set_style_opa(rpm, LV_OPA_TRANSP, 0);

    // Monogram in the middle, dashing in from the left; shine bands over it.
    lv_obj_t *mono = NULL;
    if (s_emblem_px || emblem_rasterise()) {
        const int32_t eh = s_emblem_dsc.header.h, ey = DIAL_CY - eh / 2 + 6;
        s_emblem_x = DIAL_CX - EMBLEM_W / 2;
        mono = emblem_window(scr, 0, EMBLEM_W, EMBLEM_COLOR);
        lv_obj_set_y(mono, ey);
        shine_create(scr, ey);
    }
    lv_obj_t *word = ui_label_create(scr, ui_font_md, UI_COLOR_MUTED, "D A S H C A R");
    lv_obj_set_style_text_letter_space(word, 6, 0);
    lv_obj_align(word, LV_ALIGN_TOP_MID, 0, DIAL_CY + DIAL_R / 2 + 34);

    // 0-0.6 s ticks light up; 0.55-1.2 s sweep to the end of the scale, 1.2-1.85 s back to 0;
    // 1.25-1.85 s the monogram dashes in; 1.9 s shine; "DASHCAR" fades in.
    custom_anim(dial, dial_light, 0, DIAL_TICKS, 60, 560, lv_anim_path_linear);
    animate(rpm, anim_label_opa, 0, 255, 300, 400, lv_anim_path_linear);
    custom_anim(s_dial_arc, dial_sweep, 0, 1000, 550, 650, lv_anim_path_ease_in_out);
    custom_anim(s_needle, dial_sweep, 1000, 0, 1200, 650, lv_anim_path_ease_in_out);
    if (mono) {
        animate(mono, anim_x, s_emblem_x - 120, s_emblem_x, 1250, 600, lv_anim_path_ease_out);
        animate(mono, anim_window_opa, 0, 255, 1250, 350, lv_anim_path_linear);
        custom_anim(s_shine[0], shine_move, s_emblem_x - SHINE_W, s_emblem_x + EMBLEM_W + SHINE_W,
                    1900, 600, lv_anim_path_ease_in_out);
    }
    animate(word, anim_label_opa, 0, 255, 1700, 500, lv_anim_path_linear);
}
#endif

void ui_splash_open(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    bool have_creds = settings_get_wifi(ssid, pass);

    lv_obj_t *scr = ui_screen_create();
    s_splash = scr;
    s_splash_min_done = false;
    s_splash_connected = false;
    lv_obj_add_event_cb(scr, on_splash_delete, LV_EVENT_DELETE, NULL);
    // Plain near-black: a gradient bands visibly on the RGB565 panel.
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05080E), 0);

#if DASHCAR_EMBLEM_PRIVATE
    splash_emblem_lock(scr);
#else
    splash_ignition(scr);
#endif

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

    s_splash_timer = lv_timer_create(on_splash_timer, SPLASH_MIN_MS, NULL);
    lv_timer_set_repeat_count(s_splash_timer, 1);

    if (have_creds) {
        wifi_mgr_add_listener(on_splash_wifi, NULL);
        wifi_mgr_connect(ssid, pass, true);
    }
    ui_screen_load(scr);
}
