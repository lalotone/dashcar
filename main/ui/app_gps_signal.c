// GPS signal: one bar per satellite plus a plain-language verdict, so "no fix" can be told apart
// into wiring, antenna and just-still-searching. Runs only while open (the data comes from gps.c).
#include <stdio.h>
#include <stdlib.h>
#include "services/gps.h"
#include "ui.h"

#define VERDICT_Y   (UI_HEADER_H + 8)
#define VERDICT_H   152
#define STAT_W      116
#define CHART_Y     (VERDICT_Y + VERDICT_H + 12)
#define CHART_H     (600 - CHART_Y - UI_PAD)
#define AXIS_W      64                  // left of the bars: the "usable" line's label
#define PLOT_W      (1024 - 2 * UI_PAD - 2 * UI_PAD - 2 - AXIS_W)
#define MIN_SLOTS   12                  // bars don't get wider than this many would be
#define LEGEND_H    30
#define VALUE_H     24
#define BAR_MAX_H   232
#define BAR_MAX_SNR 50                  // top of the scale: open-sky signals are about 35-50 dB-Hz
#define BAR_MIN_H   4                   // a stub, so a satellite that isn't received still shows
#define BAR_BASE_Y  (LEGEND_H + VALUE_H + BAR_MAX_H)
#define PRN_Y       (BAR_BASE_Y + 4)

#define HINT_AFTER_S  180               // a cold start legitimately takes a couple of minutes
#define MIN_FOR_FIX   4                 // satellites needed for a 3D position
#define SNR_REDRAW_DB 2                 // signals jitter by a dB: don't redraw for that

typedef enum { TONE_OK, TONE_WARN, TONE_ERR } tone_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_used;
static lv_obj_t *s_received;
static lv_obj_t *s_best;
static lv_obj_t *s_empty;
static lv_obj_t *s_bar[GPS_MAX_SATS];
static lv_obj_t *s_value[GPS_MAX_SATS];
static lv_obj_t *s_prn[GPS_MAX_SATS];
static gps_sat_t s_shown[GPS_MAX_SATS];
static int s_shown_n;
static int s_tone;

static void on_back(lv_event_t *e)
{
    ui_gps_open();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_scr) {
        return;  // a newer instance of this screen already took over the shared state
    }
    s_scr = NULL;
}

static int bar_height(int snr)
{
    const int h = (snr > BAR_MAX_SNR ? BAR_MAX_SNR : snr) * BAR_MAX_H / BAR_MAX_SNR;
    return h < BAR_MIN_H ? BAR_MIN_H : h;
}

static lv_color_t bar_color(const gps_sat_t *s)
{
    return s->used ? UI_COLOR_OK :
           s->snr >= GPS_USABLE_SNR ? UI_COLOR_ACCENT :
           s->snr > 0 ? UI_COLOR_WARN : UI_COLOR_BORDER;
}

static void verdict(gps_state_t st, const gps_signal_t *sig, tone_t *tone, const char **title, char *body, size_t len)
{
    gps_fix_t f;
    gps_get(&f);
    const uint32_t min = sig->no_fix_s / 60;

    if (st == GPS_NO_DATA) {
        *tone = TONE_ERR;
        *title = "No data from the GPS";
        snprintf(body, len, "The receiver isn't sending anything. Check its power and wiring.");
    } else if (st == GPS_FIX && f.hdop > GPS_MAX_HDOP) {
        *tone = TONE_WARN;
        *title = "Fix, but poor accuracy";
        snprintf(body, len, "Positions may wander and speed reads 0. Give the antenna a clearer view of the sky.");
    } else if (st == GPS_FIX) {
        *tone = TONE_OK;
        *title = "Good signal";
        snprintf(body, len, "Position fix from %d satellites.", f.sats > 0 ? f.sats : sig->used);
    } else if (sig->no_fix_s < HINT_AFTER_S) {
        *tone = TONE_WARN;
        *title = "Searching";
        snprintf(body, len, "Looking for satellites. A first fix can take a few minutes.");
    } else if (sig->received == 0) {
        *tone = TONE_ERR;
        *title = "No signal: antenna problem?";
        snprintf(body, len, "Nothing received for %lu min. Check the antenna connector and that it can see the sky.",
                 (unsigned long)min);
    } else if (sig->usable < MIN_FOR_FIX) {
        *tone = TONE_ERR;
        *title = "Weak signal: antenna problem?";
        snprintf(body, len, "No fix for %lu min: %d strong enough, %d needed. Check the connector and move the "
                 "antenna away from the screen, towards the windscreen.",
                 (unsigned long)min, sig->usable, MIN_FOR_FIX);
    } else {
        *tone = TONE_WARN;
        *title = "Signal OK, still searching";
        snprintf(body, len, "%d satellites are strong enough. The receiver is downloading their orbit data, "
                 "which can take a few more minutes.", sig->usable);
    }
}

static void update_bars(const gps_sat_t *sats, int n)
{
    const bool relayout = n != s_shown_n;
    const int pitch = PLOT_W / (n > MIN_SLOTS ? n : MIN_SLOTS);
    char buf[8];
    for (int i = 0; i < GPS_MAX_SATS; i++) {
        const bool on = i < n;
        ui_set_hidden_if_changed(s_bar[i], !on);
        ui_set_hidden_if_changed(s_value[i], !on);
        ui_set_hidden_if_changed(s_prn[i], !on);
        if (!on) {
            continue;
        }
        const gps_sat_t *s = &sats[i], *old = &s_shown[i];
        const bool appeared = relayout || old->prn != s->prn;
        const bool lost = (old->snr > 0) != (s->snr > 0);
        if (!appeared && !lost && old->used == s->used && abs(old->snr - s->snr) < SNR_REDRAW_DB) {
            continue;
        }
        s_shown[i] = *s;
        const int h = bar_height(s->snr), x = AXIS_W + i * pitch;
        lv_obj_set_size(s_bar[i], pitch * 3 / 5, h);
        lv_obj_set_pos(s_bar[i], x + pitch / 5, BAR_BASE_Y - h);
        lv_obj_set_style_bg_color(s_bar[i], bar_color(s), 0);
        lv_obj_set_width(s_value[i], pitch);
        lv_obj_set_pos(s_value[i], x, BAR_BASE_Y - h - VALUE_H);
        lv_obj_set_width(s_prn[i], pitch);
        lv_obj_set_pos(s_prn[i], x, PRN_Y);
        if (s->snr > 0) {
            snprintf(buf, sizeof(buf), "%d", s->snr);
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        ui_label_set_text_if_changed(s_value[i], buf);
        snprintf(buf, sizeof(buf), "%d", s->prn);
        ui_label_set_text_if_changed(s_prn[i], buf);
    }
    s_shown_n = n;
}

static void update(lv_timer_t *t)
{
    if (!s_scr) {
        return;
    }
    gps_sat_t sats[GPS_MAX_SATS];
    gps_signal_t sig;
    const int n = gps_get_sats(sats, &sig);
    int shown_sats;
    const gps_state_t st = gps_state(&shown_sats);

    if (st == GPS_NO_DATA) {  // the table is stale once the receiver goes quiet
        const uint32_t no_fix_s = sig.no_fix_s;
        sig = (gps_signal_t){ .no_fix_s = no_fix_s };
    }

    tone_t tone;
    const char *title;
    char buf[160];
    verdict(st, &sig, &tone, &title, buf, sizeof(buf));
    ui_label_set_text_if_changed(s_title, title);
    ui_label_set_text_if_changed(s_body, buf);
    if ((int)tone != s_tone) {  // restyling redraws: only on change
        lv_obj_set_style_text_color(s_title, tone == TONE_OK ? UI_COLOR_OK : tone == TONE_WARN ? UI_COLOR_WARN : UI_COLOR_ERR, 0);
        s_tone = tone;
    }

    snprintf(buf, sizeof(buf), "%d", sig.used);
    ui_label_set_text_if_changed(s_used, buf);
    snprintf(buf, sizeof(buf), "%d/%d", sig.received, sig.listed);
    ui_label_set_text_if_changed(s_received, buf);
    if (sig.best_snr > 0) {
        snprintf(buf, sizeof(buf), "%d dB", sig.best_snr);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    ui_label_set_text_if_changed(s_best, buf);

    const int shown = st == GPS_NO_DATA ? 0 : n;
    ui_set_hidden_if_changed(s_empty, shown > 0);
    update_bars(sats, shown);
}

static lv_obj_t *stat_create(lv_obj_t *parent, const char *name, int32_t x)
{
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_MUTED, name);
    lv_obj_set_pos(l, x, 12);
    lv_obj_t *v = ui_label_create(parent, ui_font_lg, UI_COLOR_TEXT, "--");
    lv_obj_set_pos(v, x, 44);
    return v;
}

static void legend_item(lv_obj_t *parent, const char *text, lv_color_t color, int32_t x)
{
    lv_obj_t *swatch = lv_obj_create(parent);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, 16, 16);
    lv_obj_set_pos(swatch, x, 3);
    lv_obj_set_style_radius(swatch, 4, 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(swatch, color, 0);
    lv_obj_set_clickable(swatch, false);
    lv_obj_t *l = ui_label_create(parent, ui_font_sm, UI_COLOR_MUTED, text);
    lv_obj_set_pos(l, x + 24, 0);
}

void ui_gps_signal_open(void)
{
    lv_obj_t *scr = ui_screen_create();
    s_scr = scr;
    s_tone = -1;
    s_shown_n = -1;
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    ui_header_create(scr, "GPS signal", on_back);

    lv_obj_t *top = ui_card_create(scr);
    lv_obj_set_size(top, 1024 - 2 * UI_PAD, VERDICT_H);
    lv_obj_set_pos(top, UI_PAD, VERDICT_Y);
    lv_obj_set_scrollable(top, false);
    lv_obj_set_clickable(top, false);
    s_title = ui_label_create(top, ui_font_lg, UI_COLOR_MUTED, "");
    s_body = ui_label_create(top, ui_font_md, UI_COLOR_TEXT, "");
    lv_obj_set_pos(s_body, 0, 38);
    lv_obj_set_width(s_body, 1024 - 4 * UI_PAD - 2 - 3 * STAT_W - UI_PAD);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    const int32_t stats_x = 1024 - 4 * UI_PAD - 2 - 3 * STAT_W;
    s_used = stat_create(top, "IN USE", stats_x);
    s_received = stat_create(top, "RECEIVED", stats_x + STAT_W);
    s_best = stat_create(top, "BEST", stats_x + 2 * STAT_W);

    lv_obj_t *chart = ui_card_create(scr);
    lv_obj_set_size(chart, 1024 - 2 * UI_PAD, CHART_H);
    lv_obj_set_pos(chart, UI_PAD, CHART_Y);
    lv_obj_set_scrollable(chart, false);
    lv_obj_set_clickable(chart, false);

    legend_item(chart, "In use", UI_COLOR_OK, AXIS_W);
    legend_item(chart, "Strong enough", UI_COLOR_ACCENT, AXIS_W + 120);
    legend_item(chart, "Too weak", UI_COLOR_WARN, AXIS_W + 320);
    legend_item(chart, "Not received", UI_COLOR_BORDER, AXIS_W + 470);

    // The line a satellite has to clear to be of any use.
    const int32_t usable_y = BAR_BASE_Y - bar_height(GPS_USABLE_SNR);
    lv_obj_t *line = lv_obj_create(chart);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, PLOT_W, 2);
    lv_obj_set_pos(line, AXIS_W, usable_y);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, UI_COLOR_MUTED, 0);
    lv_obj_set_clickable(line, false);
    char mark[8];
    snprintf(mark, sizeof(mark), "%d dB", GPS_USABLE_SNR);
    lv_obj_t *mark_l = ui_label_create(chart, ui_font_sm, UI_COLOR_MUTED, mark);
    lv_obj_set_pos(mark_l, 0, usable_y - 11);

    s_empty = ui_label_create(chart, ui_font_md, UI_COLOR_MUTED, "No satellites listed");
    lv_obj_set_pos(s_empty, AXIS_W, LEGEND_H + VALUE_H + 40);

    for (int i = 0; i < GPS_MAX_SATS; i++) {
        lv_obj_t *b = lv_obj_create(chart);
        lv_obj_remove_style_all(b);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_clickable(b, false);
        lv_obj_set_hidden(b, true);
        s_bar[i] = b;
        s_value[i] = ui_label_create(chart, ui_font_sm, UI_COLOR_TEXT, "");
        lv_obj_set_style_text_align(s_value[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(s_value[i], UI_COLOR_CARD, 0);  // keeps the "usable" line off the digits
        lv_obj_set_style_bg_opa(s_value[i], LV_OPA_COVER, 0);
        lv_obj_set_hidden(s_value[i], true);
        s_prn[i] = ui_label_create(chart, ui_font_sm, UI_COLOR_MUTED, "");
        lv_obj_set_style_text_align(s_prn[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_hidden(s_prn[i], true);
    }

    update(NULL);
    ui_screen_own_timer(scr, lv_timer_create(update, 1000, NULL));
    ui_screen_load(scr);
}
