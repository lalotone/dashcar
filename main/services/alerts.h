#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Official weather warnings (AEMET via MeteoAlarm's public Atom feed) for the warning zone(s)
// containing the forecast location. Refreshed every 30 min and when the location changes.

#define ALERTS_MAX 8

typedef enum {
    ALERT_YELLOW = 2,   // MeteoAlarm awareness levels: Moderate / Severe / Extreme
    ALERT_ORANGE = 3,
    ALERT_RED = 4,
} alert_level_t;

typedef struct {
    alert_level_t level;
    char title[48];     // "Yellow Rain Warning"
    char event[48];     // "Moderate rain warning"
    char area[64];      // "Ribera del Ebro de Zaragoza"
    time_t onset;       // UTC epoch
    time_t expires;
} alert_t;

// Runs in the alerts task: take the LVGL lock before touching UI.
typedef void (*alerts_cb_t)(void *ctx);

void alerts_init(void);
void alerts_add_listener(alerts_cb_t cb, void *ctx);
void alerts_remove_listener(alerts_cb_t cb, void *ctx);

// Current warnings (active now or starting within 24 h), most severe first. Returns count.
int alerts_get(alert_t *out, int max);
// Name of the (first) zone containing the location, "" if unknown.
void alerts_zone_name(char *buf, size_t len);
