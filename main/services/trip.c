#include "trip.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "services/fuel.h"   // fuel_distance_km
#include "services/gps.h"    // GPS_MIN_KMH
#include "services/location.h"
#include "services/net_worker.h"
#include "services/weather.h"
#include "settings.h"

static const char *TAG = "trip";

#if CONFIG_DASHCAR_SOAK
#define CACHE_KEY      "trip_soak"  // tools/soak.sh: the simulated drive must not touch the real totals
#else
#define CACHE_KEY      "trip2"  // "trip" held kilometres counted from parked GPS noise (first test build)
#endif
#define MOVING_KMH     GPS_MIN_KMH
#define MOVING_FIXES   3        // consecutive moving fixes before distance counts (one noisy fix isn't driving)
#define MAX_JUMP_KMH   250.0f   // a fix implying more than this is a GPS glitch: skip it
#define MAX_DT_S       5        // longer gaps (lost fix): don't integrate across them

typedef struct {
    trip_info_t info;
    int32_t     day;            // local day number of today_*
    int32_t     month;          // year * 12 + month of month_km
} store_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static store_t s_st;
static double s_lat, s_lon;      // previous fix
static int64_t s_prev_us;        // 0 = none
static bool s_was_moving;
static int s_moving_run;         // consecutive fixes at or above MOVING_KMH
static bool s_dirty;
static int64_t s_saved_us;
static int32_t s_utc_offset;     // refreshed by the save job (weather may take a lock)

static void local_keys(time_t now, int32_t *day, int32_t *month)
{
    const time_t t = now + s_utc_offset;
    struct tm tm;
    gmtime_r(&t, &tm);
    *day = (int32_t)(t / 86400);
    *month = (tm.tm_year + 1900) * 12 + tm.tm_mon;
}

static void job_save(void *arg)
{
    (void)arg;
    s_utc_offset = weather_utc_offset();
    store_t copy;
    portENTER_CRITICAL(&s_mux);
    copy = s_st;
    portEXIT_CRITICAL(&s_mux);
    settings_set_cache(CACHE_KEY, &copy, sizeof(copy));
}

static void save_later(int64_t now_us)
{
    s_dirty = false;
    s_saved_us = now_us;
    net_worker_post(job_save, NULL);  // NVS: not from the timer task
}

void trip_init(void)
{
    s_utc_offset = settings_get_utc_offset();
    if (!settings_get_cache(CACHE_KEY, &s_st, sizeof(s_st))) {
        memset(&s_st, 0, sizeof(s_st));
    }
}

void trip_on_fix(double lat, double lon, float kmh)
{
    const int64_t now_us = esp_timer_get_time();
    const time_t now = time(NULL);
    if (now < 1700000000) {
        return;  // the GPS sets the clock with the first fix
    }
    const int64_t prev_us = s_prev_us;
    const double plat = s_lat, plon = s_lon;
    s_prev_us = now_us;
    s_lat = lat;
    s_lon = lon;
    if (!prev_us) {
        return;
    }
    const float dt = (now_us - prev_us) / 1e6f;
    if (dt <= 0 || dt > MAX_DT_S) {
        return;
    }
    const float d_km = fuel_distance_km((float)plat, (float)plon, (float)lat, (float)lon);
    if (d_km / dt * 3600 > MAX_JUMP_KMH) {
        return;
    }
    s_moving_run = kmh >= MOVING_KMH ? s_moving_run + 1 : 0;
    const bool moving = s_moving_run >= MOVING_FIXES;
    int32_t day, month;
    local_keys(now, &day, &month);

    portENTER_CRITICAL(&s_mux);
    trip_info_t *in = &s_st.info;
    trip_t *t = &in->current;
    if (t->start && now - t->last > TRIP_GAP_S) {
        in->previous = *t;          // long pause / power-off: that trip is over
        memset(t, 0, sizeof(*t));
    }
    if (day != s_st.day) {
        s_st.day = day;
        in->today_km = 0;
        in->today_moving_s = 0;
    }
    if (month != s_st.month) {
        s_st.month = month;
        in->month_km = 0;
    }
    if (moving) {
        if (!t->start) {
            t->start = now;
        }
        t->km += d_km;
        t->moving_s += (uint32_t)lroundf(dt);
        t->max_kmh = fmaxf(t->max_kmh, kmh);
        in->today_km += d_km;
        in->today_moving_s += (uint32_t)lroundf(dt);
        in->month_km += d_km;
        in->total_km += d_km;
    } else if (t->start) {
        t->stopped_s += (uint32_t)lroundf(dt);
    }
    if (t->start) {
        t->last = now;
    }
    portEXIT_CRITICAL(&s_mux);

    s_dirty |= moving;
    const bool stopped_now = s_was_moving && !moving;
    s_was_moving = moving;
    if (stopped_now || (s_dirty && now_us - s_saved_us >= LOCATION_FOLLOW_MS * 1000LL)) {
        save_later(now_us);
    }
}

void trip_get(trip_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st.info;
    portEXIT_CRITICAL(&s_mux);
    // A trip that ended while the board was off is shown as the previous one.
    const time_t now = time(NULL);
    if (out->current.start && now > 1700000000 && now - out->current.last > TRIP_GAP_S) {
        out->previous = out->current;
        memset(&out->current, 0, sizeof(out->current));
    }
}

void trip_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_st.info.current.start) {
        s_st.info.previous = s_st.info.current;
    }
    memset(&s_st.info.current, 0, sizeof(s_st.info.current));
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "trip reset");
    save_later(esp_timer_get_time());
}
