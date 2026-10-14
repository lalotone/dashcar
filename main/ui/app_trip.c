// Trip computer: current trip (distance, driving/stopped time, average and top speed), today,
// this month, previous trip and the GPS odometer. Live (1 s); data from services/trip.
#include <math.h>
#include <stdio.h>
#include "services/trip.h"
#include "services/weather.h"
#include "settings.h"
#include "ui.h"

#define BODY_Y   (UI_HEADER_H + 8)
#define BODY_H   (600 - BODY_Y - UI_PAD)
#define LEFT_W   500
#define RIGHT_X  (UI_PAD + LEFT_W + UI_PAD)
#define RIGHT_W  (1024 - RIGHT_X - UI_PAD)
#define GAP      12

typedef enum { S_TIME, S_AVG, S_MAX, S_STOP, S_COUNT } stat_t;
typedef enum { R_TODAY, R_MONTH, R_PREV, R_TOTAL, R_COUNT } row_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_dist, *s_dist_unit, *s_since;
static lv_obj_t *s_stat[S_COUNT];
static lv_obj_t *s_row[R_COUNT];
static bool s_imp;   // units, read once per update (NVS)

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

static float dist(float km)
{
    return s_imp ? km / 1.609344f : km;
}

static const char *dist_unit(void)
{
    return s_imp ? "mi" : "km";
}

static void fmt_hm(char *buf, size_t len, uint32_t s)
{
    snprintf(buf, len, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

static void fmt_km(char *buf, size_t len, float km)
{
    const float v = dist(km);
    snprintf(buf, len, v < 100 ? "%.1f %s" : "%.0f %s", v, dist_unit());
}

static void fmt_speed(char *buf, size_t len, float kmh)
{
    snprintf(buf, len, "%ld %s", lroundf(dist(kmh)), s_imp ? "mph" : "km/h");
}

static void update(lv_timer_t *t)
{
    if (!s_scr) {
        return;
    }
    s_imp = settings_get_imperial();
    trip_info_t in;
    trip_get(&in);
    const trip_t *c = &in.current;
    char buf[128], a[32], b[32];

    const float d = dist(c->km);
    snprintf(buf, sizeof(buf), d < 100 ? "%.1f" : "%.0f", d);
    ui_label_set_text_if_changed(s_dist, buf);
    ui_label_set_text_if_changed(s_dist_unit, dist_unit());
    if (c->start) {
        ui_fmt_local(a, sizeof(a), "%a %H:%M", c->start, weather_utc_offset());
        snprintf(buf, sizeof(buf), "Started %s", a);
    } else {
        snprintf(buf, sizeof(buf), "Starts when the car moves (needs a GPS fix)");
    }
    ui_label_set_text_if_changed(s_since, buf);

    fmt_hm(buf, sizeof(buf), c->moving_s);
    ui_label_set_text_if_changed(s_stat[S_TIME], buf);
    if (c->moving_s >= 60) {
        fmt_speed(buf, sizeof(buf), c->km / c->moving_s * 3600);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_stat[S_AVG], buf);
    if (c->max_kmh > 0) {
        fmt_speed(buf, sizeof(buf), c->max_kmh);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_stat[S_MAX], buf);
    fmt_hm(buf, sizeof(buf), c->stopped_s);
    ui_label_set_text_if_changed(s_stat[S_STOP], buf);

    fmt_km(a, sizeof(a), in.today_km);
    fmt_hm(b, sizeof(b), in.today_moving_s);
    snprintf(buf, sizeof(buf), "%s  ·  %s driving", a, b);
    ui_label_set_text_if_changed(s_row[R_TODAY], buf);
    fmt_km(buf, sizeof(buf), in.month_km);
    ui_label_set_text_if_changed(s_row[R_MONTH], buf);
    if (in.previous.start) {
        fmt_km(a, sizeof(a), in.previous.km);
        fmt_hm(b, sizeof(b), in.previous.moving_s);
        char when[24];
        ui_fmt_local(when, sizeof(when), "%a %d %b", in.previous.start, weather_utc_offset());
        snprintf(buf, sizeof(buf), "%s  ·  %s  ·  %s", a, b, when);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_row[R_PREV], buf);
    fmt_km(buf, sizeof(buf), (float)in.total_km);
    ui_label_set_text_if_changed(s_row[R_TOTAL], buf);
}

static void on_reset(lv_event_t *e)
{
    trip_reset();
    ui_toast("New trip started");
    update(NULL);
}

static lv_obj_t *stat_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(48), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 2, 0);
    lv_obj_set_clickable(box, false);
    ui_label_create(box, ui_font_sm, UI_COLOR_MUTED, title);
    return ui_label_create(box, ui_font_xl, UI_COLOR_TEXT, "--");
}

void ui_trip_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_t *hdr = ui_header_create(scr, "Trip", on_back);
    ui_header_button(hdr, LV_SYMBOL_REFRESH, "New trip", on_reset, NULL);

    // Left: the current trip
    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, LEFT_W, BODY_H);
    lv_obj_set_pos(left, UI_PAD, BODY_Y);
    lv_obj_set_scrollable(left, false);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, 24, 0);
    lv_obj_set_style_pad_row(left, 4, 0);
    ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "CURRENT TRIP");
    lv_obj_t *row = lv_obj_create(left);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_clickable(row, false);
    s_dist = ui_label_create(row, ui_font_huge, UI_COLOR_TEXT, "0.0");
    s_dist_unit = ui_label_create(row, ui_font_lg, UI_COLOR_MUTED, "km");
    lv_obj_set_style_pad_bottom(s_dist_unit, 22, 0);
    s_since = ui_label_create(left, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_t *spacer = lv_obj_create(left);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);
    lv_obj_t *grid = lv_obj_create(left);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 18, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_clickable(grid, false);
    s_stat[S_TIME] = stat_create(grid, "DRIVING TIME");
    s_stat[S_AVG] = stat_create(grid, "AVERAGE SPEED");
    s_stat[S_MAX] = stat_create(grid, "TOP SPEED");
    s_stat[S_STOP] = stat_create(grid, "STOPPED");

    // Right: totals, one card per line
    static const char *const titles[R_COUNT] = { "TODAY", "THIS MONTH", "PREVIOUS TRIP", "GPS ODOMETER" };
    const int32_t h = (BODY_H - (R_COUNT - 1) * GAP) / R_COUNT;
    for (int i = 0; i < R_COUNT; i++) {
        lv_obj_t *c = ui_card_create(scr);
        lv_obj_set_size(c, RIGHT_W, h);
        lv_obj_set_pos(c, RIGHT_X, BODY_Y + i * (h + GAP));
        lv_obj_set_scrollable(c, false);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(c, 4, 0);
        ui_label_create(c, ui_font_sm, UI_COLOR_MUTED, titles[i]);
        s_row[i] = ui_label_create(c, i == R_PREV ? ui_font_md : ui_font_lg, UI_COLOR_TEXT, "--");
        lv_obj_set_width(s_row[i], lv_pct(100));
        lv_label_set_long_mode(s_row[i], LV_LABEL_LONG_DOT);
    }

    update(NULL);
    ui_screen_own_timer(scr, lv_timer_create(update, 1000, NULL));
    ui_screen_load(scr);
}
