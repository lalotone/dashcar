#include "alerts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/location.h"
#include "services/net_worker.h"
#include "services/timeutil.h"
#include "services/warn_zones.h"
#include "services/weather.h"

static const char *TAG = "alerts";

#define FEED_URL          "https://feeds.meteoalarm.org/feeds/meteoalarm-legacy-atom-spain"
#define REFRESH_MS        (10 * 60 * 1000)
#define RETRY_MS          (2 * 60 * 1000)
#define LOOKAHEAD_S       (24 * 3600)   // also show warnings starting within a day
#define MAX_ZONES         4
#define ENTRY_BUF         8192          // one <entry> is ~1.5 KB
#define MAX_LISTENERS     4

typedef struct {
    alerts_cb_t cb;
    void *ctx;
} listener_t;

static listener_t s_listeners[MAX_LISTENERS];
static SemaphoreHandle_t s_lock;
static volatile bool s_kick;   // forecast refreshed: location may have changed
static alert_t s_alerts[ALERTS_MAX];
static int s_count;
static char s_zone_name[64];

// ---------------------------------------------------------------------------
// Zones

static bool in_ring(const warn_ring_t *r, int32_t x, int32_t y)
{
    bool in = false;
    for (int i = 0, j = r->count - 1; i < r->count; j = i++) {
        const int32_t *a = WARN_PTS[r->first + i], *b = WARN_PTS[r->first + j];
        if ((a[1] > y) != (b[1] > y) &&
                x < (int64_t)(b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) {
            in = !in;
        }
    }
    return in;
}

int warn_zones_at(float lat, float lon, int *out, int max)
{
    const int32_t x = (int32_t)(lon * 1e5f), y = (int32_t)(lat * 1e5f);
    int n = 0;
    for (int z = 0; z < WARN_ZONE_COUNT && n < max; z++) {
        const warn_zone_t *zone = &WARN_ZONES[z];
        if (x < zone->bbox[0] || y < zone->bbox[1] || x > zone->bbox[2] || y > zone->bbox[3]) {
            continue;
        }
        bool in = false;  // even-odd over all rings handles holes and multipolygons
        for (int r = 0; r < zone->ring_count; r++) {
            in ^= in_ring(&WARN_RINGS[zone->ring_first + r], x, y);
        }
        if (in) {
            out[n++] = z;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Feed parsing (streamed, one <entry> at a time)

typedef struct {
    char codes[MAX_ZONES][6];
    int ncodes;
    time_t now;
    alert_t found[ALERTS_MAX];
    int nfound;
    char *buf;      // current partial text
    size_t len;
} parse_ctx_t;

// Copies the text of <tag>…</tag> within [p, end) into out. Returns false if absent.
static bool tag_text(const char *p, const char *end, const char *tag, char *out, size_t len)
{
    char open[40], close[40];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(p, open);
    if (!a || a >= end) {
        return false;
    }
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || b > end) {
        return false;
    }
    size_t n = ((size_t)(b - a) < len - 1 ? (size_t)(b - a) : len - 1);
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

static void handle_entry(parse_ctx_t *pc, const char *p, const char *end)
{
    char code[16] = "", buf[160];
    // <cap:geocode><valueName>EMMA_ID</valueName><value>ES107</value></cap:geocode>
    if (!tag_text(p, end, "value", code, sizeof(code))) {
        return;
    }
    bool ours = false;
    for (int i = 0; i < pc->ncodes; i++) {
        ours |= strcmp(code, pc->codes[i]) == 0;
    }
    if (!ours) {
        return;
    }
    alert_t a = { 0 };
    if (tag_text(p, end, "cap:expires", buf, sizeof(buf))) {
        a.expires = iso8601_to_utc(buf);
    }
    if (tag_text(p, end, "cap:onset", buf, sizeof(buf))) {
        a.onset = iso8601_to_utc(buf);
    }
    if (a.expires <= pc->now || a.onset > pc->now + LOOKAHEAD_S) {
        return;  // over, or too far ahead
    }
    tag_text(p, end, "cap:severity", buf, sizeof(buf));
    a.level = strcmp(buf, "Extreme") == 0 ? ALERT_RED : strcmp(buf, "Severe") == 0 ? ALERT_ORANGE : ALERT_YELLOW;
    tag_text(p, end, "cap:event", a.event, sizeof(a.event));
    tag_text(p, end, "cap:areaDesc", a.area, sizeof(a.area));
    // "Yellow Rain Warning issued for Spain - …" -> "Yellow Rain Warning"
    if (tag_text(p, end, "title", buf, sizeof(buf))) {
        char *cut = strstr(buf, " issued");
        if (cut) {
            *cut = '\0';
        }
        strlcpy(a.title, buf, sizeof(a.title));
    }
    for (int i = 0; i < pc->nfound; i++) {  // same warning listed twice (updates)
        if (strcmp(pc->found[i].title, a.title) == 0 && pc->found[i].onset == a.onset) {
            return;
        }
    }
    if (pc->nfound < ALERTS_MAX) {
        pc->found[pc->nfound++] = a;
    }
}

static bool on_feed_data(const char *data, size_t len, void *ctx)
{
    parse_ctx_t *pc = ctx;
    if (pc->len + len >= ENTRY_BUF) {
        // Keep only the tail (an entry never spans more than the buffer).
        size_t keep = ENTRY_BUF / 4;
        if (pc->len > keep) {
            memmove(pc->buf, pc->buf + pc->len - keep, keep);
            pc->len = keep;
        }
        if (len >= ENTRY_BUF - pc->len) {
            len = ENTRY_BUF - pc->len - 1;
        }
    }
    memcpy(pc->buf + pc->len, data, len);
    pc->len += len;
    pc->buf[pc->len] = '\0';

    char *p = pc->buf;
    for (;;) {
        char *start = strstr(p, "<entry>");
        char *end = start ? strstr(start, "</entry>") : NULL;
        if (!end) {
            break;
        }
        handle_entry(pc, start, end);
        p = end + 8;
    }
    // Drop consumed text; keep a possible partial "<entry>".
    char *rest = strstr(p, "<entry>");
    if (!rest) {
        size_t n = strlen(p);
        rest = p + (n > 8 ? n - 8 : 0);
    }
    pc->len = strlen(rest);
    memmove(pc->buf, rest, pc->len + 1);
    return true;
}

static int cmp_alert(const void *a, const void *b)
{
    const alert_t *x = a, *y = b;
    if (x->level != y->level) {
        return y->level - x->level;
    }
    return x->onset < y->onset ? -1 : x->onset > y->onset;
}

static void notify(void)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(s_listeners[i].ctx);
        }
    }
}

static bool fetch(float lat, float lon)
{
    int zones[MAX_ZONES];
    int nz = warn_zones_at(lat, lon, zones, MAX_ZONES);
    parse_ctx_t *pc = heap_caps_calloc(1, sizeof(*pc), MALLOC_CAP_SPIRAM);
    if (!pc) {
        return false;
    }
    pc->buf = heap_caps_malloc(ENTRY_BUF, MALLOC_CAP_SPIRAM);
    pc->now = time(NULL);
    for (int i = 0; i < nz; i++) {
        strlcpy(pc->codes[i], WARN_ZONES[zones[i]].code, sizeof(pc->codes[i]));
    }
    pc->ncodes = nz;
    bool ok = pc->buf != NULL;
    if (ok && nz > 0) {
        ok = http_get_stream_gzip(FEED_URL, on_feed_data, pc) == ESP_OK;
    }
    if (ok) {
        qsort(pc->found, pc->nfound, sizeof(alert_t), cmp_alert);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_alerts, pc->found, sizeof(alert_t) * pc->nfound);
        s_count = pc->nfound;
        strlcpy(s_zone_name, nz > 0 ? WARN_ZONES[zones[0]].name : "", sizeof(s_zone_name));
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "zone %s (%s): %d active warning(s)", nz ? WARN_ZONES[zones[0]].code : "-",
                 s_zone_name, pc->nfound);
    }
    free(pc->buf);
    free(pc);
    return ok;
}

static void on_weather(weather_status_t st, const char *msg, void *ctx)
{
    if (st == WEATHER_STATUS_OK) {
        s_kick = true;  // location may have changed
    }
}

// Called by the network worker about once a second.
static void alerts_step(void)
{
    static float last_lat, last_lon;
    static TickType_t last_try;
    static bool have, tried;
    location_t loc;
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED || time(NULL) < 1700000000 || !location_get(&loc)) {
        return;
    }
    const bool moved = have && (fabsf(loc.lat - last_lat) > 0.02f || fabsf(loc.lon - last_lon) > 0.02f);
    const TickType_t since = xTaskGetTickCount() - last_try;
    const bool due = !tried || since >= pdMS_TO_TICKS(have ? REFRESH_MS : RETRY_MS);
    if (!due && !(s_kick && moved)) {
        s_kick = false;
        return;
    }
    s_kick = false;
    tried = true;
    last_try = xTaskGetTickCount();
    if (fetch(loc.lat, loc.lon)) {
        have = true;
        last_lat = loc.lat;
        last_lon = loc.lon;
        notify();
    }
}

void alerts_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    net_worker_register_step(alerts_step);
    weather_add_listener(on_weather, NULL);
}

void alerts_add_listener(alerts_cb_t cb, void *ctx)
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

void alerts_remove_listener(alerts_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            s_listeners[i] = (listener_t){ 0 };
        }
    }
}

int alerts_get(alert_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        if (s_alerts[i].expires > now) {
            out[n++] = s_alerts[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

void alerts_zone_name(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_zone_name, len);
    xSemaphoreGive(s_lock);
}
