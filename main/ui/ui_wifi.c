// Wi-Fi setup: scan list + SSID/password form typed on the on-screen keyboard.
#include <stdio.h>
#include <string.h>
#include "net/wifi_mgr.h"
#include "settings.h"
#include "ui.h"

#define KB_H 260

// Only one instance exists at a time; s_scr == NULL means the screen is gone,
// which callbacks from other tasks must check after taking the LVGL lock.
static lv_obj_t *s_scr;
static lv_obj_t *s_list;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_ssid_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_connect_btn;
static lv_obj_t *s_status;
static lv_obj_t *s_spinner;
static lv_obj_t *s_kb;
static bool s_connecting;
static wifi_mgr_ap_t s_aps[24];
static bool s_keep_status;  // don't let scan results overwrite an error

static void start_scan(void);

static void set_status(const char *text, lv_color_t color, bool busy)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, color, 0);
    if (busy) {
        lv_obj_set_hidden(s_spinner, false);
    } else {
        lv_obj_set_hidden(s_spinner, true);
    }
}

static void set_form_enabled(bool en)
{
    lv_obj_t *objs[] = { s_ssid_ta, s_pass_ta, s_connect_btn, s_scan_btn };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (en) {
            lv_obj_remove_state(objs[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(objs[i], LV_STATE_DISABLED);
        }
    }
}

static void kb_hide(void)
{
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_set_hidden(s_kb, true);
    lv_obj_remove_state(s_ssid_ta, LV_STATE_FOCUSED);
    lv_obj_remove_state(s_pass_ta, LV_STATE_FOCUSED);
}

static void do_connect(void)
{
    const char *ssid = lv_textarea_get_text(s_ssid_ta);
    const char *pass = lv_textarea_get_text(s_pass_ta);
    if (ssid[0] == '\0') {
        set_status("Enter the network name (SSID)", UI_COLOR_WARN, false);
        return;
    }
    if (pass[0] != '\0' && strlen(pass) < 8) {
        set_status("Password must be at least 8 characters", UI_COLOR_WARN, false);
        return;
    }
    kb_hide();
    s_connecting = true;
    s_keep_status = false;
    set_form_enabled(false);
    char buf[80];
    snprintf(buf, sizeof(buf), "Connecting to %s...", ssid);
    set_status(buf, UI_COLOR_TEXT, true);
    wifi_mgr_connect(ssid, pass, false);
}

static void on_wifi_state(wifi_mgr_state_t st, const char *reason, void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr && s_connecting) {
        if (st == WIFI_MGR_CONNECTED) {
            s_connecting = false;
            settings_set_wifi(lv_textarea_get_text(s_ssid_ta), lv_textarea_get_text(s_pass_ta));
            ui_toast("Connected");
            ui_home_open();
        } else if (st == WIFI_MGR_FAILED || st == WIFI_MGR_IDLE) {
            s_connecting = false;
            set_form_enabled(true);
            set_status(reason ? reason : "Not connected", UI_COLOR_ERR, false);
        }
    }
    ui_unlock();
}

static void on_ap_clicked(lv_event_t *e)
{
    const wifi_mgr_ap_t *ap = lv_event_get_user_data(e);
    lv_textarea_set_text(s_ssid_ta, ap->ssid);
    lv_textarea_set_text(s_pass_ta, "");
    // Jump straight to the password field.
    lv_obj_remove_state(s_ssid_ta, LV_STATE_FOCUSED);
    lv_obj_add_state(s_pass_ta, LV_STATE_FOCUSED);
    lv_obj_send_event(s_pass_ta, LV_EVENT_FOCUSED, NULL);
}

static void on_scan_done(const wifi_mgr_ap_t *aps, int count, void *ctx)
{
    if (!ui_lock()) {
        return;
    }
    if (s_scr) {
        lv_obj_clean(s_list);
        if (count == 0) {
            ui_list_add_text(s_list, "No networks found");
        }
        if (count > (int)(sizeof(s_aps) / sizeof(s_aps[0]))) {
            count = sizeof(s_aps) / sizeof(s_aps[0]);
        }
        for (int i = 0; i < count; i++) {
            s_aps[i] = aps[i];
            const char *bars = aps[i].rssi > -60 ? "+++" : aps[i].rssi > -72 ? "++" : "+";
            lv_obj_t *b = ui_list_add_button(s_list, aps[i].secure ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI,
                                             s_aps[i].ssid, on_ap_clicked, &s_aps[i]);
            lv_obj_t *sig = ui_label_create(b, ui_font_sm, UI_COLOR_MUTED, bars);
            lv_obj_set_style_text_color(sig, aps[i].rssi > -72 ? UI_COLOR_OK : UI_COLOR_WARN, 0);
        }
        lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
        if (!s_connecting && !s_keep_status) {
            set_status(count ? "Pick a network or type its name" : "No networks found, type the name", UI_COLOR_MUTED, false);
        }
    }
    ui_unlock();
}

static void start_scan(void)
{
    lv_obj_clean(s_list);
    ui_list_add_text(s_list, "Scanning...");
    lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    if (!wifi_mgr_scan(on_scan_done, NULL)) {
        lv_obj_clean(s_list);
        ui_list_add_text(s_list, "Scan failed, try again");
        lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
    }
}

static void on_scan_clicked(lv_event_t *e)
{
    start_scan();
}

static void on_connect_clicked(lv_event_t *e)
{
    do_connect();
}

static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_set_hidden(s_kb, false);
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        // "OK" on SSID moves to password; on password it connects.
        if (lv_keyboard_get_textarea(s_kb) == s_ssid_ta) {
            lv_obj_remove_state(s_ssid_ta, LV_STATE_FOCUSED);
            lv_obj_add_state(s_pass_ta, LV_STATE_FOCUSED);
            lv_keyboard_set_textarea(s_kb, s_pass_ta);
        } else {
            do_connect();
        }
    } else if (code == LV_EVENT_CANCEL) {
        kb_hide();
    }
}

static void on_show_pass(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target_obj(e);
    lv_textarea_set_password_mode(s_pass_ta, !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

static void on_back(lv_event_t *e)
{
    // Scanning stops reconnect attempts; resume the saved network if we're offline.
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED && !s_connecting && settings_get_wifi(ssid, pass)) {
        wifi_mgr_connect(ssid, pass, true);
    }
    ui_home_open();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    wifi_mgr_remove_listener(on_wifi_state, NULL);
    s_scr = NULL;
    s_connecting = false;
}

static lv_obj_t *make_ta(lv_obj_t *parent, const char *placeholder, size_t max_len)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, ui_font_md, 0);
    lv_obj_set_style_bg_color(ta, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_ver(ta, 12, 0);
    lv_obj_add_event_cb(ta, on_ta_event, LV_EVENT_ALL, NULL);
    return ta;
}

void ui_wifi_open(bool allow_back)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    s_connecting = false;
    s_keep_status = false;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);

    ui_header_create(scr, "Wi-Fi setup", allow_back ? on_back : NULL);

    // Left: nearby networks
    lv_obj_t *left = ui_card_create(scr);
    lv_obj_set_size(left, 420, 600 - UI_HEADER_H - UI_PAD);
    lv_obj_set_pos(left, UI_PAD, UI_HEADER_H);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    lv_obj_set_scrollable(left, false);

    lv_obj_t *row = lv_obj_create(left);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label_create(row, ui_font_lg, UI_COLOR_TEXT, "Nearby networks");
    s_scan_btn = lv_button_create(row);
    lv_obj_set_size(s_scan_btn, 56, 48);
    lv_obj_set_style_bg_color(s_scan_btn, UI_COLOR_CARD_HI, 0);
    lv_obj_add_event_cb(s_scan_btn, on_scan_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_scan_btn);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);

    s_list = ui_list_create(left);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    // Right: the form
    lv_obj_t *right = ui_card_create(scr);
    lv_obj_set_size(right, 1024 - 420 - 3 * UI_PAD, 600 - UI_HEADER_H - UI_PAD);
    lv_obj_set_pos(right, 420 + 2 * UI_PAD, UI_HEADER_H);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_set_scrollable(right, false);

    ui_label_create(right, ui_font_sm, UI_COLOR_MUTED, "Network name (SSID)");
    s_ssid_ta = make_ta(right, "e.g. MyHotspot", SETTINGS_SSID_MAX - 1);

    ui_label_create(right, ui_font_sm, UI_COLOR_MUTED, "Password");
    s_pass_ta = make_ta(right, "Leave empty for open networks", SETTINGS_PASS_MAX - 1);
    lv_textarea_set_password_mode(s_pass_ta, true);

    lv_obj_t *actions = lv_obj_create(right);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *show = lv_checkbox_create(actions);
    lv_checkbox_set_text(show, "Show password");
    lv_obj_set_style_text_font(show, ui_font_sm, 0);
    lv_obj_add_event_cb(show, on_show_pass, LV_EVENT_VALUE_CHANGED, NULL);
    s_connect_btn = ui_button_create(actions, LV_SYMBOL_OK "  Connect", UI_COLOR_ACCENT, on_connect_clicked, NULL);

    lv_obj_t *status_row = lv_obj_create(right);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 12, 0);
    s_spinner = lv_spinner_create(status_row);
    lv_obj_set_size(s_spinner, 28, 28);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_INDICATOR);
    s_status = ui_label_create(status_row, ui_font_md, UI_COLOR_MUTED, "");
    lv_obj_set_flex_grow(s_status, 1);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);

    // Keyboard overlays the bottom of the screen while a field is focused.
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, lv_pct(100), KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, ui_font_md, 0);
    lv_obj_add_event_cb(s_kb, on_kb_event, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_kb, true);

    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass)) {
        lv_textarea_set_text(s_ssid_ta, ssid);
        lv_textarea_set_text(s_pass_ta, pass);
    }

    wifi_mgr_add_listener(on_wifi_state, NULL);
    ui_screen_load(scr);
    set_status("Scanning for networks...", UI_COLOR_MUTED, false);
    start_scan();
}

// Shown at boot when the saved network couldn't be joined.
void ui_wifi_open_with_error(const char *msg)
{
    ui_wifi_open(false);
    s_keep_status = true;
    set_status(msg, UI_COLOR_ERR, false);
}
