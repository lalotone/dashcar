// Settings: Wi-Fi, night mode, units, wireless updates, clock.
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "net/wifi_mgr.h"
#include "services/ota.h"
#include "services/ext_rtc.h"
#include "services/weather.h"
#include "settings.h"
#include "ui.h"

#define COL_W   ((1024 - 3 * UI_PAD) / 2)
#define TOP_Y   (UI_HEADER_H + 4)

static lv_obj_t *s_scr;
static lv_obj_t *s_wifi_sub;
static lv_obj_t *s_night_sub;
static lv_obj_t *s_night_btns[3];
static lv_obj_t *s_unit_btns[2];
static lv_obj_t *s_ota_switch;
static lv_obj_t *s_ota_sub;
static lv_obj_t *s_clock_sub;

static lv_obj_t *section(lv_obj_t *col, const char *title)
{
    lv_obj_t *card = ui_card_create(col);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F2A3D), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_scrollable(card, false);
    ui_label_create(card, ui_font_lg, UI_COLOR_TEXT, title);
    return card;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_clickable(r, false);
    return r;
}

static lv_obj_t *sub(lv_obj_t *parent)
{
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_MUTED, "");
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t value)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 44);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)value);
    lv_obj_center(ui_label_create(b, ui_font_sm, UI_COLOR_TEXT, text));
    return b;
}

static void chip_set(lv_obj_t *b, bool on)
{
    lv_obj_set_style_bg_color(b, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_HI, 0);
}

static void ip_text(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    } else {
        strlcpy(buf, "no IP", len);
    }
}

static void refresh(void)
{
    char buf[200], ip[20];
    ip_text(ip, sizeof(ip));

    switch (wifi_mgr_state()) {
    case WIFI_MGR_CONNECTED:
        snprintf(buf, sizeof(buf), "Connected to %s  ·  %s  ·  %d dBm", wifi_mgr_ssid(), ip, wifi_mgr_rssi());
        break;
    case WIFI_MGR_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting to %s...", wifi_mgr_ssid());
        break;
    default:
        snprintf(buf, sizeof(buf), "Not connected");
        break;
    }
    lv_label_set_text(s_wifi_sub, buf);

    ui_night_describe(buf, sizeof(buf));
    if (ui_night_active()) {
        strlcat(buf, "  ·  dimmed now", sizeof(buf));
    }
    lv_label_set_text(s_night_sub, buf);
    const night_mode_t nm = settings_get_night_mode();
    for (int i = 0; i < 3; i++) {
        chip_set(s_night_btns[i], (int)nm == i);
    }

    const bool imperial = settings_get_imperial();
    chip_set(s_unit_btns[0], !imperial);
    chip_set(s_unit_btns[1], imperial);

    const esp_app_desc_t *app = esp_app_get_description();
    if (ota_server_running()) {
        snprintf(buf, sizeof(buf),
                 "Version %s (%s)  ·  slot %s\nReady: http://dashcar.local  ·  %s\nFrom a computer on this network: tools/ota.sh",
                 app->version, app->date, ota_running_partition(), ip);
    } else {
        snprintf(buf, sizeof(buf), "Version %s (%s)  ·  slot %s\n%s", app->version, app->date,
                 ota_running_partition(),
                 settings_get_ota_enabled() ? "Waiting for Wi-Fi..." : "Off: firmware can only be updated over USB");
    }
    lv_label_set_text(s_ota_sub, buf);
    if (settings_get_ota_enabled()) {
        lv_obj_add_state(s_ota_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_ota_switch, LV_STATE_CHECKED);
    }

    const char *rtc;
    switch (ext_rtc_state()) {
    case EXT_RTC_VALID:
        rtc = "RTC OK: keeps time across restarts";
        break;
    case EXT_RTC_INVALID:
        rtc = "RTC lost its time (no backup cell?): set again from the network";
        break;
    default:
        rtc = "RTC not found";
        break;
    }
    time_t now = time(NULL);
    if (now > 1700000000) {
        char t[32];
        ui_fmt_local(t, sizeof(t), "%H:%M:%S", now, weather_utc_offset());
        snprintf(buf, sizeof(buf), "%s  ·  UTC%+ld\n%s", t, (long)(weather_utc_offset() / 3600), rtc);
    } else {
        snprintf(buf, sizeof(buf), "Time not set yet\n%s", rtc);
    }
    lv_label_set_text(s_clock_sub, buf);
}

static void on_timer(lv_timer_t *t)
{
    refresh();
}

static void on_night(lv_event_t *e)
{
    settings_set_night_mode((night_mode_t)(intptr_t)lv_event_get_user_data(e));
    ui_night_update();
    refresh();
}

static void on_units(lv_event_t *e)
{
    bool imperial = (intptr_t)lv_event_get_user_data(e) == 1;
    if (imperial != settings_get_imperial()) {
        settings_set_imperial(imperial);
        weather_refresh();
    }
    refresh();
}

static void on_ota_switch(lv_event_t *e)
{
    settings_set_ota_enabled(lv_obj_has_state(s_ota_switch, LV_STATE_CHECKED));
    ota_apply_setting();
    refresh();
}

static void on_wifi(lv_event_t *e)
{
    ui_wifi_open(true);
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
    s_scr = NULL;
}

static lv_obj_t *column(lv_obj_t *scr, int32_t x)
{
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, COL_W, 600 - TOP_Y - UI_PAD);
    lv_obj_set_pos(col, x, TOP_Y);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, UI_PAD, 0);
    lv_obj_set_scrollable(col, false);
    return col;
}

void ui_settings_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05080F), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    ui_header_create(scr, "Settings", on_back);

    lv_obj_t *left = column(scr, UI_PAD);
    lv_obj_t *right = column(scr, 2 * UI_PAD + COL_W);

    // Wi-Fi
    lv_obj_t *c = section(left, LV_SYMBOL_WIFI "  Wi-Fi");
    s_wifi_sub = sub(c);
    lv_obj_t *r = row(c);
    ui_button_create(r, "Change network", UI_COLOR_CARD_HI, on_wifi, NULL);

    // Night mode
    c = section(left, LV_SYMBOL_EYE_CLOSE "  Night mode");
    s_night_sub = sub(c);
    r = row(c);
    s_night_btns[NIGHT_MODE_AUTO] = chip(r, "Auto", on_night, NIGHT_MODE_AUTO);
    s_night_btns[NIGHT_MODE_ON] = chip(r, "On", on_night, NIGHT_MODE_ON);
    s_night_btns[NIGHT_MODE_OFF] = chip(r, "Off", on_night, NIGHT_MODE_OFF);

    // Units
    c = section(left, "Units");
    r = row(c);
    s_unit_btns[0] = chip(r, "°C  ·  km/h", on_units, 0);
    s_unit_btns[1] = chip(r, "°F  ·  mph", on_units, 1);

    // Wireless updates
    c = section(right, LV_SYMBOL_DOWNLOAD "  Wireless updates");
    r = row(c);
    lv_obj_t *lbl = ui_label_create(r, ui_font_md, UI_COLOR_TEXT, "Accept updates over Wi-Fi");
    lv_obj_set_flex_grow(lbl, 1);
    s_ota_switch = lv_switch_create(r);
    lv_obj_set_size(s_ota_switch, 64, 34);
    lv_obj_add_event_cb(s_ota_switch, on_ota_switch, LV_EVENT_VALUE_CHANGED, NULL);
    s_ota_sub = sub(c);

    // Clock
    c = section(right, LV_SYMBOL_BELL "  Clock");
    s_clock_sub = sub(c);

    // Data sources
    c = section(right, "Data");
    lv_obj_t *d = sub(c);
    lv_label_set_text(d, "Weather: Open-Meteo (backup: MET Norway)  ·  Warnings: AEMET via MeteoAlarm\n"
                         "Radar: RainViewer  ·  Map: Esri, © OpenStreetMap\n"
                         "Fuel: Ministerio (Spain)");

    refresh();
    ui_screen_own_timer(scr, lv_timer_create(on_timer, 1000, NULL));
    ui_screen_load(scr);
}
