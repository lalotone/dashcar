// GPS: speed, heading, position, altitude and receiver status, live (1 s).
#include <math.h>
#include <stdio.h>
#include "services/gps.h"
#include "services/speedlimit.h"
#include "settings.h"
#include "ui.h"

#define BODY_Y   (UI_HEADER_H + 8)
#define BODY_H   (600 - BODY_Y - UI_PAD)
#define LEFT_W   400
#define RIGHT_X  (UI_PAD + LEFT_W + UI_PAD)
#define TILE_GAP 12
#define TILE_W   ((1024 - RIGHT_X - UI_PAD - TILE_GAP) / 2)
#define TILE_H   ((BODY_H - TILE_GAP) / 2)

typedef enum { T_LAT, T_LON, T_ALT, T_SATS, T_COUNT } tile_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_speed;
static lv_obj_t *s_speed_unit;
static lv_obj_t *s_heading;
static lv_obj_t *s_limit;          // speed limit sign (OpenStreetMap)
static lv_obj_t *s_limit_src;
static bool s_speed_red;
static lv_obj_t *s_status;
static int s_status_state;
static lv_obj_t *s_value[T_COUNT];
static lv_obj_t *s_sub[T_COUNT];

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

static const char *compass(float deg)
{
    static const char *const names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return names[(int)lroundf(deg / 45.0f) % 8];
}

static void update(lv_timer_t *t)
{
    if (!s_scr) {
        return;
    }
    gps_fix_t f;
    const bool have = gps_get(&f);
    int sats;
    const gps_state_t st = gps_state(&sats);
    const bool live = st == GPS_FIX;
    const bool imperial = settings_get_imperial();
    char buf[64];

    // Speed and heading only make sense for a current fix.
    if (live) {
        const float v = imperial ? f.speed_kmh / 1.609344f : f.speed_kmh;
        snprintf(buf, sizeof(buf), "%ld", v < 1 ? 0L : lroundf(v));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_speed, buf);
    ui_label_set_text_if_changed(s_speed_unit, imperial ? "mph" : "km/h");

    // Speed limit of this road (signs in Spain are km/h, so the sign stays in km/h).
    speedlimit_t sl;
    speedlimit_get(&sl);
    const bool over = live && ui_speed_over_limit(f.speed_kmh, sl.kmh);
    ui_set_hidden_if_changed(s_limit, sl.kmh <= 0);
    ui_set_hidden_if_changed(s_limit_src, sl.kmh <= 0);
    if (sl.kmh > 0) {
        ui_speed_sign_set(s_limit, sl.kmh, over);
        ui_label_set_text_if_changed(s_limit_src, sl.implicit ? "road default" : "OpenStreetMap");
    }
    if (over != s_speed_red) {
        lv_obj_set_style_text_color(s_speed, over ? UI_COLOR_ERR : UI_COLOR_TEXT, 0);
        s_speed_red = over;
    }

    char road[24] = "";
    if (sl.kmh > 0 && sl.ref[0]) {
        snprintf(road, sizeof(road), "  ·  %s", sl.ref);
    }
    if (live && f.speed_kmh >= 5) {
        snprintf(buf, sizeof(buf), "Heading %s  (%ld°)%s", compass(f.course), lroundf(f.course) % 360, road);
    } else {
        snprintf(buf, sizeof(buf), live ? "Heading: stopped%s" : "Heading: -%s", road);
    }
    ui_label_set_text_if_changed(s_heading, buf);

    gps_status_text(buf, sizeof(buf));
    if (have && !live) {
        char age[96];
        const uint32_t m = f.age_s / 60;
        snprintf(age, sizeof(age), "%s\nLast fix %lu %s ago", buf, (unsigned long)(m ? m : f.age_s), m ? "min" : "s");
        ui_label_set_text_if_changed(s_status, age);
    } else {
        ui_label_set_text_if_changed(s_status, buf);
    }
    if ((int)st != s_status_state) {  // restyling redraws: only on change
        lv_obj_set_style_text_color(s_status, live ? UI_COLOR_OK : st == GPS_SEARCHING ? UI_COLOR_WARN : UI_COLOR_MUTED, 0);
        s_status_state = st;
    }

    // Position: keep showing the last fix (marked by the status line) rather than blanking it.
    if (have) {
        snprintf(buf, sizeof(buf), "%.5f°", fabs(f.lat));
        ui_label_set_text_if_changed(s_value[T_LAT], buf);
        ui_label_set_text_if_changed(s_sub[T_LAT], f.lat >= 0 ? "North" : "South");
        snprintf(buf, sizeof(buf), "%.5f°", fabs(f.lon));
        ui_label_set_text_if_changed(s_value[T_LON], buf);
        ui_label_set_text_if_changed(s_sub[T_LON], f.lon >= 0 ? "East" : "West");
    } else {
        for (int i = T_LAT; i <= T_LON; i++) {
            ui_label_set_text_if_changed(s_value[i], "--");
            ui_label_set_text_if_changed(s_sub[i], "waiting for a fix");
        }
    }
    if (have && f.has_alt) {
        snprintf(buf, sizeof(buf), "%ld %s", lroundf(imperial ? f.alt_m * 3.28084f : f.alt_m), imperial ? "ft" : "m");
        ui_label_set_text_if_changed(s_value[T_ALT], buf);
        ui_label_set_text_if_changed(s_sub[T_ALT], "above sea level");
    } else {
        ui_label_set_text_if_changed(s_value[T_ALT], "--");
        ui_label_set_text_if_changed(s_sub[T_ALT], "needs a 3D fix");
    }
    if (sats >= 0) {
        snprintf(buf, sizeof(buf), "%d", sats);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_value[T_SATS], buf);
    if (live && f.hdop > 0) {
        // HDOP: below ~2 is a good fix, above ~5 poor (open sky vs. city canyon / inside a garage).
        snprintf(buf, sizeof(buf), "used  ·  accuracy %s (HDOP %.1f)",
                 f.hdop < 2 ? "good" : f.hdop < 5 ? "fair" : "poor", f.hdop);
    } else {
        snprintf(buf, sizeof(buf), st == GPS_SEARCHING ? "in view, searching" : "no data");
    }
    ui_label_set_text_if_changed(s_sub[T_SATS], buf);
}

static void tile_create(lv_obj_t *scr, tile_t id, const char *title, int32_t x, int32_t y)
{
    lv_obj_t *c = ui_card_create(scr);
    lv_obj_set_size(c, TILE_W, TILE_H);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    ui_label_create(c, ui_font_sm, UI_COLOR_MUTED, title);
    lv_obj_t *spacer = lv_obj_create(c);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    s_value[id] = ui_label_create(c, ui_font_xl, UI_COLOR_TEXT, "--");
    s_sub[id] = ui_label_create(c, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_set_width(s_sub[id], lv_pct(100));
    lv_label_set_long_mode(s_sub[id], LV_LABEL_LONG_WRAP);
}

void ui_gps_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    s_status_state = -1;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    ui_header_create(scr, "GPS", on_back);

    // Left: speed (the number a driver glances at), heading, receiver status.
    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, BODY_Y);
    lv_obj_set_scrollable(left, false);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, 24, 0);
    lv_obj_set_style_pad_row(left, 4, 0);
    ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "SPEED");
    s_speed = ui_label_create(left, ui_font_huge, UI_COLOR_TEXT, "--");
    s_speed_unit = ui_label_create(left, ui_font_lg, UI_COLOR_MUTED, "km/h");
    lv_obj_t *gap = lv_obj_create(left);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, 18);
    s_heading = ui_label_create(left, ui_font_md, UI_COLOR_TEXT, "");
    lv_obj_t *spacer = lv_obj_create(left);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    s_status = ui_label_create(left, ui_font_md, UI_COLOR_MUTED, "");
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);

    // Speed limit sign at the top right of the speed card (hidden while unknown).
    s_limit = ui_speed_sign_create(scr, 120);
    lv_obj_set_pos(s_limit, UI_PAD + LEFT_W - 24 - 120, BODY_Y + 24);
    lv_obj_set_hidden(s_limit, true);
    s_limit_src = ui_label_create(scr, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_set_width(s_limit_src, 150);
    lv_obj_set_style_text_align(s_limit_src, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_limit_src, s_limit, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_hidden(s_limit_src, true);
    s_speed_red = false;

    // Right: 2x2 tiles
    const int32_t y2 = BODY_Y + TILE_H + TILE_GAP, x2 = RIGHT_X + TILE_W + TILE_GAP;
    tile_create(scr, T_LAT, "LATITUDE", RIGHT_X, BODY_Y);
    tile_create(scr, T_LON, "LONGITUDE", x2, BODY_Y);
    tile_create(scr, T_ALT, "ALTITUDE", RIGHT_X, y2);
    tile_create(scr, T_SATS, "SATELLITES", x2, y2);

    update(NULL);
    ui_screen_own_timer(scr, lv_timer_create(update, 1000, NULL));
    ui_screen_load(scr);
}
