#pragma once

#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

// Palette
#define UI_COLOR_BG        lv_color_hex(0x0B1220)
#define UI_COLOR_CARD      lv_color_hex(0x141C2E)
#define UI_COLOR_CARD_HI   lv_color_hex(0x1E293B)
#define UI_COLOR_BORDER    lv_color_hex(0x263247)
#define UI_COLOR_TEXT      lv_color_hex(0xE5E7EB)
#define UI_COLOR_MUTED     lv_color_hex(0x94A3B8)
#define UI_COLOR_ACCENT    lv_color_hex(0x3B82F6)
#define UI_COLOR_OK        lv_color_hex(0x22C55E)
#define UI_COLOR_WARN      lv_color_hex(0xF59E0B)
#define UI_COLOR_ERR       lv_color_hex(0xEF4444)

#define UI_HEADER_H        64
#define UI_PAD             16
#define UI_STATUS_W        300   // top-right area reserved for the global status bar

// Runtime TTF fonts (full Latin-1 + LVGL symbols via fallback)
extern const lv_font_t *ui_font_sm;    // 18
extern const lv_font_t *ui_font_md;    // 22
extern const lv_font_t *ui_font_lg;    // 28 semibold
extern const lv_font_t *ui_font_xl;    // 44 semibold
extern const lv_font_t *ui_font_display; // 64 semibold
extern const lv_font_t *ui_font_clock;   // 80 (home clock)
extern const lv_font_t *ui_font_huge;  // 112

// Call once after the LVGL display is registered (with the LVGL lock held).
void ui_init(lv_display_t *disp);

bool ui_lock(void);
void ui_unlock(void);

// New empty screen with the dashboard background.
lv_obj_t *ui_screen_create(void);
// Loads `scr` and deletes the previous screen (its LV_EVENT_DELETE handlers do cleanup).
void ui_screen_load(lv_obj_t *scr);
// Deletes `t` together with `scr`. Use this for screen timers: a screen can be opened again
// before the previous instance is deleted (double tap), so a shared static timer pointer would
// make the old instance delete the new one's timer.
void ui_screen_own_timer(lv_obj_t *scr, lv_timer_t *t);

// Header row. With back_cb, the arrow + title form one large back button (full header height,
// filling the free width) and a swipe right anywhere on `scr` also calls back_cb.
// Returns the header container; extra buttons can be added (laid out after the title).
lv_obj_t *ui_header_create(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb);
lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, const char *text, lv_event_cb_t cb, void *user_data);

lv_obj_t *ui_card_create(lv_obj_t *parent);
lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
// Every redraw costs PSRAM bandwidth the display needs: only touch widgets when something changed.
void ui_label_set_text_if_changed(lv_obj_t *label, const char *text);
void ui_set_hidden_if_changed(lv_obj_t *obj, bool hidden);
lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user_data);

// Scrollable flex-column list (replacement for the deprecated lv_list).
lv_obj_t *ui_list_create(lv_obj_t *parent);
void ui_list_add_text(lv_obj_t *list, const char *text);
lv_obj_t *ui_list_add_button(lv_obj_t *list, const char *symbol, const char *text, lv_event_cb_t cb, void *user_data);

void ui_toast(const char *msg);

// Night mode: dark overlay on the system layer (the backlight can't dim). Re-evaluated every
// 30 s; call ui_night_update() after changing the setting to apply it immediately.
void ui_night_update(void);
bool ui_night_active(void);
// "Auto · dims 20:31–07:42" style description of the current night-mode schedule.
void ui_night_describe(char *buf, size_t len);

// OTA progress overlay; matches ota_cb_t (services/ota.h). Called from the HTTP server task.
#include "services/ota.h"
void ui_ota_event(ota_event_t ev, int percent, const char *msg);

// Tells the status bar whether home is showing: elsewhere it shows the clock and a home icon,
// and tapping it goes home.
void ui_status_set_on_home(bool on_home);
bool ui_is_home(void);

// Formats a UTC epoch in the forecast location's local time (strftime format).
void ui_fmt_local(char *buf, size_t len, const char *fmt, time_t utc, int32_t utc_offset);
// Rounds and prints a temperature like "21°" or "-3°".
void ui_fmt_temp(char *buf, size_t len, float t);

// Screens
void ui_wifi_open(bool allow_back);
void ui_wifi_open_with_error(const char *msg);
void ui_splash_open(void);
void ui_home_open(void);
void ui_weather_open(void);
void ui_city_search_open(void);
void ui_radar_open(void);
void ui_settings_open(void);
void ui_traffic_open(void);
void ui_parking_open(void);
#include "services/parking.h"
// "2 h ago  ·  Thu 18:05" (or "Demo spot").
void ui_parking_age(const parking_spot_t *p, char *buf, size_t len);
// Shared traffic helpers (app_traffic.c): severity colour and "A-68 km 246.5 · Zaragoza".
#include "services/traffic.h"
lv_color_t ui_traffic_color(int sev);
void ui_traffic_where(char *buf, size_t len, const traffic_item_t *t);
// Incident detail popup (with Maps QR) on the top layer; usable from any screen.
void ui_traffic_show_detail(const traffic_item_t *t);
void ui_fuel_open(void);
// Home tile subtitle from the last fuel search ("Diésel from 1,459 €"); false if none.
bool ui_fuel_tile_text(char *buf, size_t len);
// Fuel prices refresh in the background (every 30 min) once this is called.
void ui_fuel_init(void);
// Nearest station from the last fuel search (LVGL lock held). False if none yet.
#include "services/fuel.h"
bool ui_fuel_nearest(fuel_station_t *out, int *product, time_t *fetched_at);
