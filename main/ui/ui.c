#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_lv_adapter.h"
#include "ui/touch_input.h"
#include "net/wifi_mgr.h"
#include "services/weather.h"
#include "settings.h"

extern const uint8_t font_medium_start[] asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t font_medium_end[]   asm("_binary_Montserrat_Medium_ttf_end");
extern const uint8_t font_semibold_start[] asm("_binary_Montserrat_SemiBold_ttf_start");
extern const uint8_t font_semibold_end[]   asm("_binary_Montserrat_SemiBold_ttf_end");

const lv_font_t *ui_font_sm;
const lv_font_t *ui_font_md;
const lv_font_t *ui_font_lg;
const lv_font_t *ui_font_xl;
const lv_font_t *ui_font_display;
const lv_font_t *ui_font_clock;
const lv_font_t *ui_font_huge;

static lv_obj_t *s_status_time;
static lv_obj_t *s_status_wifi;
static lv_obj_t *s_status_home;
static bool s_on_home;
static lv_obj_t *s_night;

#define NIGHT_OPA        140          // ~55 % black over everything
#define NIGHT_MARGIN_S   (20 * 60)    // after sunset / before sunrise (it's still light at sunset)
#define NIGHT_FALLBACK_START 21       // local hours, if the forecast has no sun times yet
#define NIGHT_FALLBACK_END   7

static const lv_font_t *ttf(bool bold, int32_t size, const lv_font_t *symbols)
{
    const uint8_t *start = bold ? font_semibold_start : font_medium_start;
    const uint8_t *end = bold ? font_semibold_end : font_medium_end;
    // Bigger glyph cache than the default 128 (text-heavy lists re-rasterised glyphs), and no
    // kerning lookups: the font subset has no kerning data anyway.
    lv_font_t *f = lv_tiny_ttf_create_data_ex(start, end - start, size, LV_FONT_KERNING_NONE, 256);
    // The TTF has no LV_SYMBOL_* glyphs; borrow them from the built-in Montserrat.
    f->fallback = symbols;
    return f;
}

bool ui_lock(void)
{
    return esp_lv_adapter_lock(-1) == ESP_OK;
}

void ui_unlock(void)
{
    esp_lv_adapter_unlock();
}

void ui_fmt_local(char *buf, size_t len, const char *fmt, time_t utc, int32_t utc_offset)
{
    time_t t = utc + utc_offset;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, fmt, &tm);
}

void ui_fmt_temp(char *buf, size_t len, float t)
{
    long v = lroundf(t);
    snprintf(buf, len, "%ld°", v == 0 ? 0L : v);  // avoid "-0°"
}

static void status_update(lv_timer_t *t)
{
    char buf[64];
    time_t now = time(NULL);
    if (now > 1700000000) {
        ui_fmt_local(buf, sizeof(buf), "%H:%M", now, weather_utc_offset());
        ui_label_set_text_if_changed(s_status_time, buf);
    } else {
        ui_label_set_text_if_changed(s_status_time, "--:--");
    }

    // Signal level with 3 dB hysteresis so the colour doesn't flicker around a threshold.
    static int level = -1;  // 0 good, 1 fair, 2 weak; -1 = not connected
    static lv_color_t shown;
    static bool shown_valid;
    lv_color_t color;
    switch (wifi_mgr_state()) {
    case WIFI_MGR_CONNECTED: {
        // Levels: good > -67 dBm, fair > -78 dBm, weak below. Move to a better level only
        // 3 dB past its boundary, and to a worse one only 3 dB below the current boundary.
        const int rssi = wifi_mgr_rssi();
        const int target = rssi > -67 ? 0 : rssi > -78 ? 1 : 2;
        static const int lower[] = { -67, -78 };  // lower bound of levels 0 and 1
        if (level < 0) {
            level = target;
        } else if (target < level && rssi > lower[target] + 3) {
            level = target;
        } else if (target > level && rssi < lower[level] - 3) {
            level = target;
        }
        color = level == 0 ? UI_COLOR_OK : level == 1 ? UI_COLOR_WARN : UI_COLOR_ERR;
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  %s", wifi_mgr_ssid());
        break;
    }
    case WIFI_MGR_CONNECTING:
        level = -1;
        color = UI_COLOR_WARN;
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Connecting...");
        break;
    default:
        level = -1;
        color = UI_COLOR_MUTED;
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Offline");
        break;
    }
    ui_label_set_text_if_changed(s_status_wifi, buf);
    if (!shown_valid || !lv_color_eq(shown, color)) {
        lv_obj_set_style_text_color(s_status_wifi, color, 0);
        shown = color;
        shown_valid = true;
    }
}

static void on_status_bar(lv_event_t *e)
{
    if (!s_on_home) {
        ui_home_open();
    }
}

static void status_bar_create(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_STATUS_W, UI_HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -UI_PAD, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 20, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_style_radius(bar, 18, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_PRESSED);
    // The whole status bar is the "home" button on every screen except home itself.
    lv_obj_add_event_cb(bar, on_status_bar, LV_EVENT_CLICKED, NULL);

    s_status_home = lv_label_create(bar);
    lv_label_set_text(s_status_home, LV_SYMBOL_HOME);
    lv_obj_set_style_text_font(s_status_home, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status_home, UI_COLOR_TEXT, 0);
    s_status_wifi = ui_label_create(bar, ui_font_sm, UI_COLOR_MUTED, "");
    lv_label_set_long_mode(s_status_wifi, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(s_status_wifi, UI_STATUS_W - 150, 0);
    s_status_time = ui_label_create(bar, ui_font_lg, UI_COLOR_TEXT, "--:--");

    lv_timer_t *t = lv_timer_create(status_update, 1000, NULL);
    lv_timer_ready(t);
}

// Night window for "now": [start, end) in UTC epoch seconds, or false if unknown.
static bool night_window(time_t now, time_t *start, time_t *end)
{
    time_t rise, set;
    if (weather_sun_times(now, &rise, &set)) {
        // Today's darkness: before (sunrise - margin) or after (sunset + margin).
        *start = set + NIGHT_MARGIN_S;
        *end = rise - NIGHT_MARGIN_S;
        return true;
    }
    return false;
}

static bool night_now(void)
{
    switch (settings_get_night_mode()) {
    case NIGHT_MODE_ON:
        return true;
    case NIGHT_MODE_OFF:
        return false;
    default:
        break;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        return false;  // no clock yet
    }
    time_t start, end;
    if (night_window(now, &start, &end)) {
        return now >= start || now < end;
    }
    time_t local = now + weather_utc_offset();
    struct tm tm;
    gmtime_r(&local, &tm);
    return tm.tm_hour >= NIGHT_FALLBACK_START || tm.tm_hour < NIGHT_FALLBACK_END;
}

void ui_night_update(void)
{
    lv_obj_set_hidden(s_night, !night_now());
}

bool ui_night_active(void)
{
    return s_night && !lv_obj_is_hidden(s_night);
}

void ui_night_describe(char *buf, size_t len)
{
    switch (settings_get_night_mode()) {
    case NIGHT_MODE_ON:
        snprintf(buf, len, "Always on");
        return;
    case NIGHT_MODE_OFF:
        snprintf(buf, len, "Off");
        return;
    default:
        break;
    }
    time_t now = time(NULL), start, end;
    if (now > 1700000000 && night_window(now, &start, &end)) {
        char a[8], b[8];
        ui_fmt_local(a, sizeof(a), "%H:%M", start, weather_utc_offset());
        ui_fmt_local(b, sizeof(b), "%H:%M", end, weather_utc_offset());
        snprintf(buf, len, "Auto  ·  after sunset (%s) until sunrise (%s)", a, b);
    } else {
        snprintf(buf, len, "Auto  ·  %02d:00–%02d:00 until the forecast has sun times",
                 NIGHT_FALLBACK_START, NIGHT_FALLBACK_END);
    }
}

static void night_timer(lv_timer_t *t)
{
    ui_night_update();
}

static void night_create(void)
{
    s_night = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_night);
    lv_obj_set_size(s_night, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_night, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_night, NIGHT_OPA, 0);
    lv_obj_set_clickable(s_night, false);  // touches go through
    lv_obj_set_hidden(s_night, true);
    lv_timer_t *t = lv_timer_create(night_timer, 30 * 1000, NULL);
    lv_timer_ready(t);
}

void ui_init(lv_display_t *disp)
{
    ui_font_sm = ttf(false, 18, &lv_font_montserrat_14);
    ui_font_md = ttf(false, 22, &lv_font_montserrat_20);
    ui_font_lg = ttf(true, 28, &lv_font_montserrat_28);
    ui_font_xl = ttf(true, 44, &lv_font_montserrat_40);
    ui_font_display = ttf(true, 64, &lv_font_montserrat_40);
    ui_font_clock = ttf(false, 80, &lv_font_montserrat_40);
    ui_font_huge = ttf(false, 112, &lv_font_montserrat_40);

    lv_theme_t *th = lv_theme_default_init(disp, UI_COLOR_ACCENT, lv_palette_main(LV_PALETTE_CYAN), true, ui_font_md);
    lv_display_set_theme(disp, th);
    status_bar_create();
    night_create();
}

// A swipe that started on a button must not also count as a tap on it when the finger lifts.

static void swallow_click_after_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_wait_release(indev);
    }
}

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UI_COLOR_TEXT, 0);
    lv_obj_set_scrollable(scr, false);
    lv_obj_add_event_cb(scr, swallow_click_after_gesture, LV_EVENT_GESTURE, NULL);
    return scr;
}

static void delete_owned_timer(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

void ui_screen_own_timer(lv_obj_t *scr, lv_timer_t *t)
{
    lv_obj_add_event_cb(scr, delete_owned_timer, LV_EVENT_DELETE, t);
}

void ui_screen_load(lv_obj_t *scr)
{
    touch_input_guard(350);
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}

static void back_async(void *cb)
{
    ((lv_event_cb_t)cb)(NULL);  // back handlers don't use the event
}

static void on_back_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) {
        return;
    }
    // Don't steal horizontal drags from controls that use them.
    lv_obj_t *obj = lv_indev_get_active_obj();
    if (obj && (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_keyboard_class) ||
                lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_switch_class))) {
        return;
    }
    // Navigating here would delete this screen while LVGL is still dispatching the gesture
    // to it (double free in lv_event cleanup). Run it after the event instead, and ignore the
    // rest of this press.
    lv_indev_wait_release(indev);
    lv_async_call(back_async, lv_event_get_user_data(e));
}

lv_obj_t *ui_header_create(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), UI_HEADER_H);
    lv_obj_set_style_pad_left(hdr, UI_PAD / 2, 0);
    lv_obj_set_style_pad_right(hdr, UI_STATUS_W + UI_PAD, 0);
    lv_obj_set_style_pad_column(hdr, 12, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(hdr, false);

    if (!back_cb) {
        lv_obj_t *t = ui_label_create(hdr, ui_font_lg, UI_COLOR_TEXT, title);
        lv_obj_set_style_pad_left(t, UI_PAD / 2, 0);
        lv_obj_set_flex_grow(t, 1);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        return hdr;
    }

    // Driving-friendly back: the arrow *and* the title form one button, the full header
    // height, filling the space left of the other header buttons.
    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_height(back, UI_HEADER_H - 8);
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_pad_left(back, 4, 0);
    lv_obj_set_style_pad_column(back, 14, 0);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *badge = lv_obj_create(back);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 48, 48);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, UI_COLOR_CARD_HI, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_clickable(badge, false);
    lv_obj_t *arrow = lv_label_create(badge);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);

    lv_obj_t *t = ui_label_create(back, ui_font_lg, UI_COLOR_TEXT, title);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

    // Swipe right anywhere on the screen = back.
    lv_obj_add_event_cb(scr, on_back_gesture, LV_EVENT_GESTURE, (void *)back_cb);
    return hdr;
}

lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(header);
    lv_obj_set_height(btn, 48);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_HI, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s%s", symbol ? symbol : "", symbol && text ? "  " : "", text ? text : "");
    lv_obj_t *l = ui_label_create(btn, ui_font_sm, UI_COLOR_TEXT, buf);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *ui_card_create(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_color(card, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    return card;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

void ui_label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_set_hidden_if_changed(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_is_hidden(obj) != hidden) {
        lv_obj_set_hidden(obj, hidden);
    }
}

lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 56);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_pad_hor(btn, 24, 0);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *l = ui_label_create(btn, ui_font_md, lv_color_white(), text);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *ui_list_create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

void ui_list_add_text(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = ui_label_create(list, ui_font_sm, UI_COLOR_MUTED, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_pad_all(l, 12, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

lv_obj_t *ui_list_add_button(lv_obj_t *list, const char *symbol, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_size(btn, lv_pct(100), 56);
    lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_HI, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    if (symbol) {
        ui_label_create(btn, &lv_font_montserrat_20, UI_COLOR_MUTED, symbol);
    }
    lv_obj_t *l = ui_label_create(btn, ui_font_md, UI_COLOR_TEXT, text);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return btn;
}

bool ui_is_home(void)
{
    return s_on_home;
}

void ui_status_set_on_home(bool on_home)
{
    s_on_home = on_home;
    lv_obj_set_hidden(s_status_time, on_home);   // home shows its own big clock
    lv_obj_set_hidden(s_status_home, on_home);
}

void ui_toast(const char *msg)
{
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, UI_COLOR_CARD_HI, 0);
    lv_obj_set_style_border_color(t, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(t, 24, 0);
    lv_obj_set_style_pad_hor(t, 28, 0);
    lv_obj_set_style_pad_ver(t, 14, 0);
    lv_obj_set_clickable(t, false);
    ui_label_create(t, ui_font_md, UI_COLOR_TEXT, msg);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_delete_delayed(t, 2500);
}

// ---------------------------------------------------------------------------
// OTA progress overlay (top layer, survives screen changes)

static lv_obj_t *s_ota_bg;
static lv_obj_t *s_ota_bar;
static lv_obj_t *s_ota_label;

static void ota_overlay_create(void)
{
    s_ota_bg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ota_bg);
    lv_obj_set_size(s_ota_bg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ota_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ota_bg, LV_OPA_70, 0);
    lv_obj_set_clickable(s_ota_bg, true);  // block touches while flashing

    lv_obj_t *card = ui_card_create(s_ota_bg);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    ui_label_create(card, ui_font_lg, UI_COLOR_TEXT, LV_SYMBOL_DOWNLOAD "  Installing update");
    s_ota_bar = lv_bar_create(card);
    lv_obj_set_size(s_ota_bar, lv_pct(100), 14);
    lv_bar_set_range(s_ota_bar, 0, 100);
    s_ota_label = ui_label_create(card, ui_font_md, UI_COLOR_MUTED, "Receiving firmware over Wi-Fi...");
}

static void ota_overlay_delete(void)
{
    if (s_ota_bg) {
        lv_obj_delete(s_ota_bg);
        s_ota_bg = NULL;
    }
}

static void ota_overlay_fade(lv_timer_t *t)
{
    ota_overlay_delete();
}

void ui_ota_event(ota_event_t ev, int percent, const char *msg)
{
    if (!ui_lock()) {
        return;
    }
    char buf[96];
    switch (ev) {
    case OTA_EVT_START:
        ota_overlay_delete();
        ota_overlay_create();
        break;
    case OTA_EVT_PROGRESS:
        if (s_ota_bg) {
            lv_bar_set_value(s_ota_bar, percent, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "Receiving firmware over Wi-Fi...  %d%%", percent);
            lv_label_set_text(s_ota_label, buf);
        }
        break;
    case OTA_EVT_DONE:
        if (s_ota_bg) {
            lv_bar_set_value(s_ota_bar, 100, LV_ANIM_OFF);
            lv_label_set_text(s_ota_label, "Update installed  ·  restarting...");
            lv_obj_set_style_text_color(s_ota_label, UI_COLOR_OK, 0);
        }
        break;
    case OTA_EVT_FAILED:
        if (s_ota_bg) {
            snprintf(buf, sizeof(buf), "Update failed: %s", msg ? msg : "?");
            lv_label_set_text(s_ota_label, buf);
            lv_obj_set_style_text_color(s_ota_label, UI_COLOR_ERR, 0);
            lv_timer_t *t = lv_timer_create(ota_overlay_fade, 4000, NULL);
            lv_timer_set_repeat_count(t, 1);
        }
        break;
    }
    ui_unlock();
}
