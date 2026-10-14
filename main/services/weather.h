#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define WEATHER_HOURS 24
#define WEATHER_DAYS  7
#define WEATHER_RAIN_SLOTS 8   // 15-minute precipitation slots (next 2 hours)

typedef struct {
    float temp;
    float feels_like;
    float humidity;
    float wind_speed;
    int   wind_dir;
    float precipitation;
    int   code;          // WMO weather code
    bool  is_day;
} weather_now_t;

typedef struct {
    time_t time;         // UTC epoch
    float  temp;
    int    precip_prob;  // %, or -1 if the provider has none (MET Norway in Spain): show `precip`
    float  precip;       // mm (or inch) in this hour
    int    code;
    bool   is_day;
} weather_hour_t;

typedef struct {
    time_t date;         // UTC epoch of local midnight
    float  temp_max;
    float  temp_min;
    int    precip_prob;  // %, or -1 (see weather_hour_t)
    float  precip;       // mm (or inch) over the day
    int    code;
    float  uv_max;
    time_t sunrise;
    time_t sunset;
} weather_day_t;

typedef struct {
    bool  valid;
    bool  imperial;
    char  source[16];     // "Open-Meteo" or "MET Norway" (backup when Open-Meteo refuses, e.g. HTTP 429)
    char  place[96];
    float lat, lon;
    int32_t utc_offset;   // seconds, for the forecast location
    time_t fetched_at;    // UTC epoch (0 if the clock wasn't set yet)
    weather_now_t  now;
    weather_hour_t hours[WEATHER_HOURS];
    int            hour_count;
    weather_day_t  days[WEATHER_DAYS];
    int            day_count;
    float          rain15[WEATHER_RAIN_SLOTS];  // mm (or inch) per 15 min, [0] = current slot
    int            rain15_count;
} weather_t;

typedef enum {
    WEATHER_STATUS_LOADING,
    WEATHER_STATUS_OK,
    WEATHER_STATUS_ERROR,
} weather_status_t;

typedef struct {
    char  name[96];       // "Madrid, Community of Madrid, Spain"
    float lat, lon;
} weather_place_t;

// Runs in the weather task: take the LVGL lock before touching UI.
typedef void (*weather_cb_t)(weather_status_t status, const char *msg, void *ctx);

void weather_init(void);
void weather_add_listener(weather_cb_t cb, void *ctx);
void weather_remove_listener(weather_cb_t cb, void *ctx);

void weather_refresh(void);                // async
bool weather_get(weather_t *out);          // copy of the latest data; false if none yet
weather_status_t weather_status(const char **msg);

// Blocking (network). Call from a worker task, not the LVGL task.
int weather_search_places(const char *query, weather_place_t *out, int max);

const char *weather_code_text(int code);
// "No rain expected in the next 2 hours", "Light rain starting in ~30 min", ...
void weather_rain_summary(const weather_t *w, char *buf, size_t len);
// Threshold above which a 15-minute slot counts as rain (in the data's units).
float weather_rain_threshold(const weather_t *w);
int32_t weather_utc_offset(void);
// Sunrise/sunset (UTC epoch) of the forecast day containing `now`; false if unknown.
bool weather_sun_times(time_t now, time_t *sunrise, time_t *sunset);
// Location of the current forecast (cheap; no copy of the whole forecast). Prefer location_get().
bool weather_location(float *lat, float *lon, char *name, size_t name_len);          // last known offset of the forecast location
