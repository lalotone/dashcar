#include "weather.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/net_worker.h"
#include "settings.h"

static const char *TAG = "weather";

#define REFRESH_PERIOD_MS   (10 * 60 * 1000)
#define RETRY_PERIOD_MS     (60 * 1000)        // first retry; doubles per failure...
#define RETRY_MAX_MS        (15 * 60 * 1000)   // ...up to this (e.g. a daily quota on a shared mobile IP)
#define OM_PAUSE_MS         (60 * 60 * 1000)   // skip Open-Meteo this long after MET had to stand in
#define CACHE_KEY           "wx_cache"
#define CACHE_MAX_AGE_S     (12 * 3600)
#define MAX_LISTENERS       4

typedef struct {
    weather_cb_t cb;
    void *ctx;
} listener_t;

static listener_t s_listeners[MAX_LISTENERS];
static weather_t *s_data;             // in PSRAM
static SemaphoreHandle_t s_lock;
static volatile bool s_kick = true;   // fetch at the next step (boot, weather_refresh())
static TickType_t s_last;
static weather_status_t s_status = WEATHER_STATUS_LOADING;
static int s_fails;                   // consecutive failed fetches (retry back-off)
static bool s_om_paused;              // Open-Meteo failed and MET Norway worked: use MET for a while
static TickType_t s_om_paused_at;
static char s_msg[96];

// IP geolocation result, cached for the session.
static int32_t s_cached_offset;

static bool s_ip_loc_valid;
static settings_location_t s_ip_loc;

static void notify(weather_status_t st, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = st;
    strlcpy(s_msg, msg ? msg : "", sizeof(s_msg));
    xSemaphoreGive(s_lock);
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(st, msg, s_listeners[i].ctx);
        }
    }
}

static double num(const cJSON *obj, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static double arr_num(const cJSON *obj, const char *key, int i, double def)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, key);
    const cJSON *it = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, i) : NULL;
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static int arr_len(const cJSON *obj, const char *key)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0;
}

static void join_place(char *out, size_t len, const char *a, const char *b, const char *c)
{
    out[0] = '\0';
    const char *parts[] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        if (!parts[i] || !parts[i][0]) {
            continue;
        }
        // Skip a region identical to the city (e.g. "Madrid, Madrid").
        if (i > 0 && parts[i - 1] && strcmp(parts[i], parts[i - 1]) == 0) {
            continue;
        }
        if (out[0]) {
            strlcat(out, ", ", len);
        }
        strlcat(out, parts[i], len);
    }
}

static bool locate_by_ip(settings_location_t *loc)
{
    char *body = NULL;
    bool ok = false;

    if (http_get("https://ipinfo.io/json", &body, NULL) == ESP_OK) {
        cJSON *root = cJSON_Parse(body);
        const char *ll = root ? str(root, "loc") : NULL;
        if (ll && sscanf(ll, "%f,%f", &loc->lat, &loc->lon) == 2) {
            join_place(loc->name, sizeof(loc->name), str(root, "city"), str(root, "region"), str(root, "country"));
            ok = true;
        }
        cJSON_Delete(root);
        free(body);
        body = NULL;
    }
    if (!ok && http_get("http://ip-api.com/json/?fields=status,city,regionName,country,lat,lon", &body, NULL) == ESP_OK) {
        cJSON *root = cJSON_Parse(body);
        const char *status = root ? str(root, "status") : NULL;
        if (status && strcmp(status, "success") == 0) {
            loc->lat = num(root, "lat", 0);
            loc->lon = num(root, "lon", 0);
            join_place(loc->name, sizeof(loc->name), str(root, "city"), str(root, "regionName"), str(root, "country"));
            ok = true;
        }
        cJSON_Delete(root);
        free(body);
    }
    loc->valid = ok;
    if (ok) {
        ESP_LOGI(TAG, "IP location: %s (%.3f, %.3f)", loc->name, loc->lat, loc->lon);
    }
    return ok;
}

static bool parse_forecast(const char *json, weather_t *w)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(cur) || !cJSON_IsObject(hourly) || !cJSON_IsObject(daily)) {
        cJSON_Delete(root);
        return false;
    }

    w->utc_offset = (int32_t)num(root, "utc_offset_seconds", 0);

    w->now.temp = num(cur, "temperature_2m", 0);
    w->now.feels_like = num(cur, "apparent_temperature", w->now.temp);
    w->now.humidity = num(cur, "relative_humidity_2m", 0);
    w->now.wind_speed = num(cur, "wind_speed_10m", 0);
    w->now.wind_dir = (int)num(cur, "wind_direction_10m", 0);
    w->now.precipitation = num(cur, "precipitation", 0);
    w->now.code = (int)num(cur, "weather_code", 0);
    w->now.is_day = num(cur, "is_day", 1) != 0;

    w->hour_count = arr_len(hourly, "time");
    if (w->hour_count > WEATHER_HOURS) {
        w->hour_count = WEATHER_HOURS;
    }
    for (int i = 0; i < w->hour_count; i++) {
        weather_hour_t *h = &w->hours[i];
        h->time = (time_t)arr_num(hourly, "time", i, 0);
        h->temp = arr_num(hourly, "temperature_2m", i, 0);
        h->precip_prob = (int)arr_num(hourly, "precipitation_probability", i, 0);
        h->code = (int)arr_num(hourly, "weather_code", i, 0);
        h->is_day = arr_num(hourly, "is_day", i, 1) != 0;
    }

    w->day_count = arr_len(daily, "time");
    if (w->day_count > WEATHER_DAYS) {
        w->day_count = WEATHER_DAYS;
    }
    for (int i = 0; i < w->day_count; i++) {
        weather_day_t *d = &w->days[i];
        d->date = (time_t)arr_num(daily, "time", i, 0);
        d->temp_max = arr_num(daily, "temperature_2m_max", i, 0);
        d->temp_min = arr_num(daily, "temperature_2m_min", i, 0);
        d->precip_prob = (int)arr_num(daily, "precipitation_probability_max", i, 0);
        d->code = (int)arr_num(daily, "weather_code", i, 0);
        d->uv_max = arr_num(daily, "uv_index_max", i, 0);
        d->sunrise = (time_t)arr_num(daily, "sunrise", i, 0);
        d->sunset = (time_t)arr_num(daily, "sunset", i, 0);
    }
    const cJSON *m15 = cJSON_GetObjectItemCaseSensitive(root, "minutely_15");
    w->rain15_count = cJSON_IsObject(m15) ? arr_len(m15, "precipitation") : 0;
    if (w->rain15_count > WEATHER_RAIN_SLOTS) {
        w->rain15_count = WEATHER_RAIN_SLOTS;
    }
    for (int i = 0; i < w->rain15_count; i++) {
        w->rain15[i] = arr_num(m15, "precipitation", i, 0);
    }
    cJSON_Delete(root);
    return true;
}

// ---------------------------------------------------------------------------
// MET Norway (api.met.no locationforecast/2.0 "complete"): backup provider. Free, no key, and its
// limits are per app rather than per IP, so it still answers when Open-Meteo returns HTTP 429 to a
// shared mobile-carrier IP. No precipitation probability in Spain, no 15-min nowcast, no time zone.

// "2026-10-08T21:00:00Z" -> UTC epoch (newlib has no timegm).
static time_t parse_iso_utc(const char *s)
{
    int y, mo, d, h, mi, sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return 0;
    }
    y -= mo <= 2;  // days from civil (H. Hinnant)
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60 + sec;
}

// MET symbol ("lightrainshowers_day") -> WMO code (what the icons and texts use).
static int met_symbol_to_wmo(const char *sym, bool *is_day)
{
    if (!sym) {
        return 3;
    }
    if (is_day) {
        *is_day = !strstr(sym, "_night");
    }
    static const struct {
        const char *prefix;
        int code;
    } map[] = {
        // Longest prefixes first: "heavyrainshowers" before "heavyrain" before "rain".
        { "heavyrainshowersandthunder", 99 }, { "heavyrainandthunder", 99 },
        { "heavysleetshowersandthunder", 99 }, { "heavysleetandthunder", 99 },
        { "heavysnowshowersandthunder", 99 }, { "heavysnowandthunder", 99 },
        { "clearsky", 0 }, { "fair", 1 }, { "partlycloudy", 2 }, { "cloudy", 3 }, { "fog", 45 },
        { "lightrainshowers", 80 }, { "heavyrainshowers", 82 }, { "rainshowers", 81 },
        { "lightrain", 61 }, { "heavyrain", 65 }, { "rain", 63 },
        { "lightsleetshowers", 66 }, { "heavysleetshowers", 67 }, { "sleetshowers", 66 },
        { "lightsleet", 66 }, { "heavysleet", 67 }, { "sleet", 67 },
        { "lightsnowshowers", 85 }, { "heavysnowshowers", 86 }, { "snowshowers", 85 },
        { "lightsnow", 71 }, { "heavysnow", 75 }, { "snow", 73 },
    };
    if (strstr(sym, "thunder")) {
        return 95;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strncmp(sym, map[i].prefix, strlen(map[i].prefix)) == 0) {
            return map[i].code;
        }
    }
    return 3;
}

// Sunrise/sunset (UTC epoch) for the day around `noon` (UTC epoch near local noon). Standard
// sunrise equation, accurate to a minute or two; false in polar day/night.
static bool sun_times(time_t noon, double lat, double lon, time_t *rise, time_t *set)
{
    const double rad = M_PI / 180;
    const double n = round(noon / 86400.0 + 2440587.5 - 2451545.0 + 0.0008);
    const double j_star = n - lon / 360;
    const double m = fmod(357.5291 + 0.98560028 * j_star, 360);
    const double c = 1.9148 * sin(m * rad) + 0.02 * sin(2 * m * rad) + 0.0003 * sin(3 * m * rad);
    const double lambda = fmod(m + c + 180 + 102.9372, 360);
    const double transit = 2451545.0 + j_star + 0.0053 * sin(m * rad) - 0.0069 * sin(2 * lambda * rad);
    const double sin_dec = sin(lambda * rad) * sin(23.4397 * rad);
    const double cos_dec = cos(asin(sin_dec));
    const double cos_w = (sin(-0.833 * rad) - sin(lat * rad) * sin_dec) / (cos(lat * rad) * cos_dec);
    if (cos_w < -1 || cos_w > 1) {
        return false;
    }
    const double w = acos(cos_w) / rad;
    *rise = (time_t)((transit - w / 360 - 2440587.5) * 86400);
    *set = (time_t)((transit + w / 360 - 2440587.5) * 86400);
    return true;
}

static bool parse_met(const char *json, weather_t *w, float lat, float lon, int32_t offset, bool imperial)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *props = root ? cJSON_GetObjectItemCaseSensitive(root, "properties") : NULL;
    const cJSON *series = props ? cJSON_GetObjectItemCaseSensitive(props, "timeseries") : NULL;
    if (!cJSON_IsArray(series) || cJSON_GetArraySize(series) == 0) {
        cJSON_Delete(root);
        return false;
    }
    const float wind_k = imperial ? 2.23694f : 3.6f;  // m/s -> mph / km/h
    const float rain_k = imperial ? 1 / 25.4f : 1;     // mm -> inch
#define TEMP(c) (imperial ? (c) * 9 / 5 + 32 : (c))

    w->utc_offset = offset;  // MET has no time zone: the last one Open-Meteo gave (UTC if never)
    const time_t now = time(NULL);
    const time_t day0 = ((now + offset) / 86400) * 86400 - offset;  // UTC epoch of local midnight
    for (int i = 0; i < WEATHER_DAYS; i++) {
        weather_day_t *d = &w->days[i];
        d->date = day0 + (time_t)i * 86400;
        d->temp_max = -1000;
        d->temp_min = 1000;
        d->precip_prob = -1;
        sun_times(d->date + 43200, lat, lon, &d->sunrise, &d->sunset);
    }

    bool have_now = false;
    const cJSON *e;
    cJSON_ArrayForEach(e, series) {
        const time_t t = parse_iso_utc(str(e, "time"));
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(e, "data");
        const cJSON *inst = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(data, "instant"), "details");
        const cJSON *n1 = cJSON_GetObjectItemCaseSensitive(data, "next_1_hours");
        const cJSON *n6 = cJSON_GetObjectItemCaseSensitive(data, "next_6_hours");
        if (!t || !inst || t + 3600 <= now) {
            continue;  // past hours
        }
        const cJSON *step = n1 ? n1 : n6;  // hourly for ~2.5 days, then 6-hourly
        const cJSON *step_sum = cJSON_GetObjectItemCaseSensitive(step, "summary");
        const cJSON *step_det = cJSON_GetObjectItemCaseSensitive(step, "details");
        bool is_day = true;
        const int code = met_symbol_to_wmo(str(step_sum, "symbol_code"), &is_day);
        const float temp = num(inst, "air_temperature", 0);
        const float precip = num(step_det, "precipitation_amount", 0) * rain_k;

        if (!have_now) {
            have_now = true;
            w->now.temp = TEMP(temp);
            w->now.feels_like = TEMP(num(inst, "apparent_air_temperature", temp));
            w->now.humidity = num(inst, "relative_humidity", 0);
            w->now.wind_speed = num(inst, "wind_speed", 0) * wind_k;
            w->now.wind_dir = (int)num(inst, "wind_from_direction", 0);
            w->now.precipitation = n1 ? precip : 0;
            w->now.code = code;
            w->now.is_day = is_day;
        }
        if (n1 && w->hour_count < WEATHER_HOURS) {
            weather_hour_t *h = &w->hours[w->hour_count++];
            h->time = t;
            h->temp = TEMP(temp);
            h->precip_prob = -1;
            h->precip = precip;
            h->code = code;
            h->is_day = is_day;
        }

        const int di = (int)((t - day0) / 86400);
        if (di < 0 || di >= WEATHER_DAYS) {
            continue;
        }
        weather_day_t *d = &w->days[di];
        float hi = temp, lo = temp;
        if (!n1 && n6) {  // 6-hourly steps carry the period's extremes
            hi = num(step_det, "air_temperature_max", temp);
            lo = num(step_det, "air_temperature_min", temp);
        }
        d->temp_max = fmaxf(d->temp_max, TEMP(hi));
        d->temp_min = fminf(d->temp_min, TEMP(lo));
        d->precip += precip;
        d->code = code > d->code ? code : d->code;  // most severe, like Open-Meteo's daily code
        d->uv_max = fmaxf(d->uv_max, num(inst, "ultraviolet_index_clear_sky", 0));
        w->day_count = di + 1;
    }
#undef TEMP
    cJSON_Delete(root);
    // Drop days without data (the end of the forecast).
    while (w->day_count > 0 && w->days[w->day_count - 1].temp_max < -999) {
        w->day_count--;
    }
    w->rain15_count = 0;  // no nowcast outside the Nordics: the UI falls back to the radar hint
    return have_now && w->hour_count > 0 && w->day_count > 0;
}

static void fetch(void)
{
    notify(WEATHER_STATUS_LOADING, "Updating...");

    settings_location_t loc;
    settings_get_location(&loc);
    if (!loc.valid) {
        if (!s_ip_loc_valid) {
            settings_location_t ip;
            if (locate_by_ip(&ip)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);  // read by weather_location() from other tasks
                s_ip_loc = ip;
                s_ip_loc_valid = true;
                xSemaphoreGive(s_lock);
            }
        }
        if (!s_ip_loc_valid) {
            s_fails++;
            notify(WEATHER_STATUS_ERROR, "Couldn't find your location. Pick a city.");
            return;
        }
        loc = s_ip_loc;
    }

    const bool imperial = settings_get_imperial();
    char url[640];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,precipitation,"
             "weather_code,wind_speed_10m,wind_direction_10m"
             "&hourly=temperature_2m,precipitation_probability,weather_code,is_day&forecast_hours=%d"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
             "uv_index_max,sunrise,sunset&forecast_days=%d"
             "&minutely_15=precipitation&forecast_minutely_15=%d"
             "&timezone=auto&timeformat=unixtime%s",
             loc.lat, loc.lon, WEATHER_HOURS, WEATHER_DAYS, WEATHER_RAIN_SLOTS,
             imperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch" : "");

    weather_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) {
        s_fails++;
        notify(WEATHER_STATUS_ERROR, "Out of memory");
        return;
    }
    if (s_om_paused && xTaskGetTickCount() - s_om_paused_at >= pdMS_TO_TICKS(OM_PAUSE_MS)) {
        s_om_paused = false;  // try the main provider again
    }
    char *body = NULL;
    bool ok = false;
    if (!s_om_paused && http_get(url, &body, NULL) == ESP_OK) {
        ok = parse_forecast(body, w);
        strlcpy(w->source, "Open-Meteo", sizeof(w->source));
    }
    free(body);
    body = NULL;
    if (!ok) {
        // Backup: MET Norway (max 4 decimals in the coordinates, or it answers 403).
        memset(w, 0, sizeof(*w));
        snprintf(url, sizeof(url), "https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=%.4f&lon=%.4f",
                 loc.lat, loc.lon);
        if (http_get(url, &body, NULL) == ESP_OK) {
            ok = parse_met(body, w, loc.lat, loc.lon, s_cached_offset, imperial);
            strlcpy(w->source, "MET Norway", sizeof(w->source));
        }
        free(body);
        if (ok && !s_om_paused) {
            ESP_LOGW(TAG, "Open-Meteo failed: using MET Norway for the next hour");
            s_om_paused = true;
            s_om_paused_at = xTaskGetTickCount();
        }
    }
    if (!ok) {
        free(w);
        s_fails++;
        notify(WEATHER_STATUS_ERROR, "Weather services unreachable");
        return;
    }
    w->valid = true;
    w->imperial = imperial;
    w->lat = loc.lat;
    w->lon = loc.lon;
    strlcpy(w->place, loc.name, sizeof(w->place));
    time_t now = time(NULL);
    w->fetched_at = now > 1700000000 ? now : 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!strcmp(w->source, "MET Norway") && s_data->valid && s_data->day_count && w->day_count &&
        s_data->days[0].date == w->days[0].date && s_data->lat == w->lat && s_data->lon == w->lon &&
        s_data->imperial == w->imperial) {
        // MET starts at the current hour, so late in the day "today" would be just the remaining
        // hours: keep the extremes of the earlier forecast for the same day and place.
        weather_day_t *d = &w->days[0];
        const weather_day_t *old = &s_data->days[0];
        d->temp_max = fmaxf(d->temp_max, old->temp_max);
        d->temp_min = fminf(d->temp_min, old->temp_min);
        d->uv_max = fmaxf(d->uv_max, old->uv_max);
    }
    xSemaphoreGive(s_lock);

    if (w->utc_offset != s_cached_offset) {
        s_cached_offset = w->utc_offset;
        settings_set_utc_offset(w->utc_offset);  // local time at next boot, before the first fetch
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_data, w, sizeof(*w));
    xSemaphoreGive(s_lock);
    if (w->fetched_at) {
        settings_set_cache(CACHE_KEY, w, sizeof(*w));  // shown at the next boot until a fetch succeeds
    }
    free(w);
    s_fails = 0;

    ESP_LOGI(TAG, "%s: %.1f, code %d (%s)", loc.name, s_data->now.temp, s_data->now.code, s_data->source);
    notify(WEATHER_STATUS_OK, NULL);
}

// Called by the network worker about once a second.
static void weather_step(void)
{
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED) {
        return;
    }
    uint32_t period = REFRESH_PERIOD_MS;
    if (s_fails) {
        period = RETRY_PERIOD_MS << (s_fails < 5 ? s_fails - 1 : 4);
        period = period < RETRY_MAX_MS ? period : RETRY_MAX_MS;
    }
    if (!s_kick && xTaskGetTickCount() - s_last < pdMS_TO_TICKS(period)) {
        return;
    }
    s_kick = false;
    s_last = xTaskGetTickCount();
    fetch();
}

void weather_init(void)
{
    s_cached_offset = settings_get_utc_offset();
    s_lock = xSemaphoreCreateMutex();
    s_data = heap_caps_calloc(1, sizeof(*s_data), MALLOC_CAP_SPIRAM);
    assert(s_lock && s_data);
    // Last good forecast from flash: the dashboard has weather right away, even with no signal or
    // when the free API is rate-limited. A fetch still runs as soon as Wi-Fi is up.
    const time_t now = time(NULL);
    if (settings_get_cache(CACHE_KEY, s_data, sizeof(*s_data))) {
        const bool usable = s_data->valid && s_data->imperial == settings_get_imperial() && now > 1700000000 &&
                            now - s_data->fetched_at < CACHE_MAX_AGE_S;
        if (usable) {
            ESP_LOGI(TAG, "cached forecast from %ld min ago", (long)(now - s_data->fetched_at) / 60);
        } else {
            memset(s_data, 0, sizeof(*s_data));
        }
    }
    net_worker_register_step(weather_step);
}

void weather_add_listener(weather_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            return;  // already registered (screen reopened before the old one was deleted)
        }
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i] = (listener_t){ cb, ctx };
            return;
        }
    }
}

void weather_remove_listener(weather_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            s_listeners[i] = (listener_t){ 0 };
        }
    }
}

void weather_refresh(void)
{
    s_kick = true;
}

bool weather_get(weather_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_data, sizeof(*out));
    xSemaphoreGive(s_lock);
    return out->valid;
}

weather_status_t weather_status(const char **msg)
{
    if (msg) {
        *msg = s_msg;
    }
    return s_status;
}

int32_t weather_utc_offset(void)
{
    return s_data && s_data->valid ? s_data->utc_offset : s_cached_offset;
}

int weather_search_places(const char *query, weather_place_t *out, int max)
{
    char q[192];
    http_url_encode(query, q, sizeof(q));
    char url[320];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json", q, max);

    char *body = NULL;
    if (http_get(url, &body, NULL) != ESP_OK) {
        return -1;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return -1;
    }
    int n = 0;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *r;
    cJSON_ArrayForEach(r, results) {
        if (n >= max) {
            break;
        }
        join_place(out[n].name, sizeof(out[n].name), str(r, "name"), str(r, "admin1"), str(r, "country"));
        out[n].lat = num(r, "latitude", 0);
        out[n].lon = num(r, "longitude", 0);
        n++;
    }
    cJSON_Delete(root);
    return n;
}

float weather_rain_threshold(const weather_t *w)
{
    return w->imperial ? 0.002f : 0.05f;  // ~0.2 mm/h
}

void weather_rain_summary(const weather_t *w, char *buf, size_t len)
{
    const float thr = weather_rain_threshold(w);
    const float heavy = w->imperial ? 0.04f : 1.0f;     // ~4 mm/h
    const float moderate = w->imperial ? 0.01f : 0.25f; // ~1 mm/h
    int n = w->rain15_count;
    float peak = 0;
    int first = -1, first_dry = -1;
    for (int i = 0; i < n; i++) {
        if (w->rain15[i] >= thr && first < 0) {
            first = i;
        }
        if (w->rain15[i] < thr && first_dry < 0) {
            first_dry = i;
        }
        peak = fmaxf(peak, w->rain15[i]);
    }
    const char *kind = peak >= heavy ? "Heavy rain" : peak >= moderate ? "Rain" : "Light rain";
    if (n == 0) {
        snprintf(buf, len, "No short-term rain data");
    } else if (first < 0) {
        snprintf(buf, len, "No rain expected in the next 2 hours");
    } else if (first == 0 && first_dry < 0) {
        snprintf(buf, len, "%s for the next 2 hours", kind);
    } else if (first == 0) {
        snprintf(buf, len, "%s stopping in ~%d min", kind, first_dry * 15);
    } else {
        snprintf(buf, len, "%s starting in ~%d min", kind, first * 15);
    }
}

const char *weather_code_text(int code)
{
    switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Violent showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Thunderstorm, hail";
    default: return "Unknown";
    }
}

bool weather_sun_times(time_t now, time_t *sunrise, time_t *sunset)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; s_data->valid && i < s_data->day_count; i++) {
        const weather_day_t *d = &s_data->days[i];
        if (now >= d->date && now < d->date + 86400 && d->sunrise && d->sunset) {
            *sunrise = d->sunrise;
            *sunset = d->sunset;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool weather_location(float *lat, float *lon, char *name, size_t name_len)
{
    // Without a forecast (no signal yet, API rate-limited) fall back to the chosen city or the
    // IP location, so maps, fuel and traffic don't depend on the weather service.
    settings_location_t chosen;
    settings_get_location(&chosen);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const settings_location_t *fallback = chosen.valid ? &chosen : s_ip_loc_valid ? &s_ip_loc : NULL;
    bool ok = true;
    if (s_data->valid) {
        *lat = s_data->lat;
        *lon = s_data->lon;
        if (name) {
            strlcpy(name, s_data->place, name_len);
        }
    } else if (fallback) {
        *lat = fallback->lat;
        *lon = fallback->lon;
        if (name) {
            strlcpy(name, fallback->name, name_len);
        }
    } else {
        ok = false;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
