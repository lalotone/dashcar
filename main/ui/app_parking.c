// "Where I parked": QR with walking directions back to the car.
#include <stdio.h>
#include <string.h>
#include "services/fuel.h"   // fuel_distance_km
#include "services/location.h"
#include "services/parking.h"
#include "services/weather.h"
#include "ui.h"

#define BODY_Y  (UI_HEADER_H + 8)
#define BODY_H  (600 - BODY_Y - UI_PAD)
#define LEFT_W  460

static lv_obj_t *s_scr;

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

static lv_obj_t *muted(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = ui_label_create(parent, font, UI_COLOR_MUTED, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

// "Saved 2 h ago  ·  Thu 18:05"
void ui_parking_age(const parking_spot_t *p, char *buf, size_t len)
{
    if (p->demo || !p->saved_at) {
        snprintf(buf, len, "Demo spot");
        return;
    }
    const long mins = (long)(time(NULL) - p->saved_at) / 60;
    char when[24];
    ui_fmt_local(when, sizeof(when), "%a %H:%M", p->saved_at, weather_utc_offset());
    if (mins < 60) {
        snprintf(buf, len, "%ld min ago  ·  %s", mins < 0 ? 0 : mins, when);
    } else if (mins < 48 * 60) {
        snprintf(buf, len, "%ld h ago  ·  %s", mins / 60, when);
    } else {
        snprintf(buf, len, "%ld days ago  ·  %s", mins / (24 * 60), when);
    }
}

void ui_parking_open(void)
{
    parking_spot_t p;
    parking_get(&p);

    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    ui_header_create(scr, "Where I parked", on_back);

    // Left: big QR with walking directions
    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, 14, 0);
    lv_obj_set_scrollable(left, false);
    char url[128];
    parking_walk_url(&p, url, sizeof(url));
    lv_obj_t *qr = lv_qrcode_create(left);
    lv_qrcode_set_size(qr, 320);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_clickable(qr, false);
    ui_label_create(left, ui_font_md, UI_COLOR_TEXT, LV_SYMBOL_GPS "  Scan to walk back to your car");
    ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "Opens Google Maps walking directions");

    // Right: details
    lv_obj_t *right = ui_card_create(scr);
    lv_obj_set_size(right, 1024 - LEFT_W - 3 * UI_PAD, BODY_H);
    lv_obj_set_pos(right, LEFT_W + 2 * UI_PAD, BODY_Y);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_set_scrollable(right, false);

    char buf[128];
    ui_label_create(right, ui_font_sm, UI_COLOR_MUTED, "PARKING SPOT");
    ui_parking_age(&p, buf, sizeof(buf));
    ui_label_create(right, ui_font_lg, UI_COLOR_TEXT, buf);
    snprintf(buf, sizeof(buf), "%.5f, %.5f", p.lat, p.lon);
    ui_label_create(right, ui_font_md, UI_COLOR_TEXT, buf);

    location_t loc;
    if (!p.demo && location_get(&loc)) {
        const float km = fuel_distance_km(loc.lat, loc.lon, (float)p.lat, (float)p.lon);
        snprintf(buf, sizeof(buf), km < 1 ? "%.0f m away (%s location)" : "%.1f km away (%s location)",
                 km < 1 ? km * 1000 : km, location_source_name(loc.source));
        muted(right, ui_font_sm, buf);
    }

    if (p.demo) {
        lv_obj_t *note = lv_obj_create(right);
        lv_obj_remove_style_all(note);
        lv_obj_set_size(note, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(note, lv_color_hex(0x3B2F0B), 0);
        lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(note, 12, 0);
        lv_obj_set_style_pad_all(note, 14, 0);
        lv_obj_set_flex_flow(note, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(note, 6, 0);
        lv_obj_set_clickable(note, false);
        ui_label_create(note, ui_font_md, UI_COLOR_WARN, LV_SYMBOL_WARNING "  No GPS module yet");
        lv_obj_t *t = ui_label_create(note, ui_font_sm, UI_COLOR_TEXT,
                                      "This is a placeholder spot (Plaza del Pilar, Zaragoza). Once the GPS "
                                      "is fitted, the spot is saved automatically when you park.");
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *spacer = lv_obj_create(right);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    muted(right, ui_font_sm, "Tip: scan before you walk away. The dashboard has no power once the car is off, "
                             "but your phone keeps the route.");

    ui_screen_load(scr);
}
