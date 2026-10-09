#include "traffic.h"

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
#include "services/fuel.h"       // fuel_distance_km
#include "services/location.h"
#include "services/net_worker.h"
#include "services/timeutil.h"

static const char *TAG = "traffic";

#define FEED_URL       "https://nap.dgt.es/datex2/v3/dgt/SituationPublication/datex2_v37.xml"
#define REFRESH_MS     (5 * 60 * 1000)
#define RETRY_MS       (2 * 60 * 1000)
#define KEEP_KM        200          // keep incidents this close to the fetch location
#define MAX_ITEMS      200
#define SIT_BUF        (64 * 1024)  // one <sit:situation> is up to ~52 KB (big roadworks)
#define MAX_LISTENERS  4

typedef struct {
    traffic_cb_t cb;
    void *ctx;
} listener_t;

static listener_t s_listeners[MAX_LISTENERS];
static SemaphoreHandle_t s_lock;
static volatile bool s_kick;
static traffic_item_t *s_items;   // PSRAM, MAX_ITEMS
static int s_count;
static time_t s_updated;

// ---------------------------------------------------------------------------
// Labels

typedef struct {
    const char *value;   // DATEX II enum value
    const char *label;
    traffic_sev_t sev;
} label_t;

static const label_t LABELS[] = {
    // causes / detailed causes
    { "accident", "Accident", TRAFFIC_HIGH },
    { "slowTraffic", "Slow traffic", TRAFFIC_MEDIUM },
    { "abnormalTraffic", "Slow traffic", TRAFFIC_MEDIUM },
    { "vehicleStuck", "Broken-down vehicle", TRAFFIC_MEDIUM },
    { "vehicleOnFire", "Vehicle on fire", TRAFFIC_HIGH },
    { "vehicleCarryingHazardousMaterials", "Hazardous load", TRAFFIC_HIGH },
    { "vehicleWithOverwideLoad", "Wide load", TRAFFIC_LOW },
    { "objectOnTheRoad", "Object on the road", TRAFFIC_MEDIUM },
    { "obstructionOnTheRoad", "Obstruction", TRAFFIC_MEDIUM },
    { "shedLoad", "Lost load", TRAFFIC_MEDIUM },
    { "rockfalls", "Rockfall", TRAFFIC_MEDIUM },
    { "flooding", "Flooding", TRAFFIC_HIGH },
    { "forestFire", "Forest fire", TRAFFIC_HIGH },
    { "avalanches", "Avalanche", TRAFFIC_HIGH },
    { "damagedRoadSurface", "Damaged road", TRAFFIC_LOW },
    { "roadSurfaceInPoorCondition", "Poor road surface", TRAFFIC_LOW },
    { "badWeather", "Bad weather", TRAFFIC_HIGH },
    { "smokeHazard", "Smoke", TRAFFIC_HIGH },
    { "roadworks", "Roadworks", TRAFFIC_LOW },
    { "speedRestrictionInOperation", "Speed restriction", TRAFFIC_LOW },
    { "driveCarefully", "Drive carefully", TRAFFIC_LOW },
    // lane / carriageway management
    { "roadClosed", "Road closed", TRAFFIC_HIGH },
    { "carriagewayClosures", "Carriageway closed", TRAFFIC_HIGH },
    { "laneClosures", "Lane closed", TRAFFIC_LOW },
    { "singleAlternateLineTraffic", "Alternating traffic", TRAFFIC_LOW },
    { "narrowLanes", "Narrow lanes", TRAFFIC_LOW },
    { "intermittentShortTermClosures", "Short closures", TRAFFIC_LOW },
    { "lanesDeviated", "Lanes diverted", TRAFFIC_LOW },
    { "newRoadworksLayout", "New road layout", TRAFFIC_LOW },
    { "weightRestrictionInOperation", "Weight limit", TRAFFIC_LOW },
    { "useOfSpecifiedLanesOrCarriagewaysAllowed", "Lane restrictions", TRAFFIC_LOW },
    { "doNotUseSpecifiedLanesOrCarriageways", "Lane restrictions", TRAFFIC_LOW },
};

static const label_t *label_for(const char *v)
{
    for (size_t i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++) {
        if (strcmp(LABELS[i].value, v) == 0) {
            return &LABELS[i];
        }
    }
    return NULL;
}

static const char *direction(const char *v)
{
    static const struct { const char *in, *out; } D[] = {
        { "northBound", "northbound" }, { "southBound", "southbound" }, { "eastBound", "eastbound" },
        { "westBound", "westbound" }, { "northEastBound", "NE-bound" }, { "northWestBound", "NW-bound" },
        { "southEastBound", "SE-bound" }, { "southWestBound", "SW-bound" },
    };
    for (size_t i = 0; i < sizeof(D) / sizeof(D[0]); i++) {
        if (strcmp(D[i].in, v) == 0) {
            return D[i].out;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Parsing (one <sit:situation> at a time)

// Text of the first <tag>…</tag> in [p, end). Returns pointer past it, or NULL.
static const char *tag_text(const char *p, const char *end, const char *tag, char *out, size_t len)
{
    char open[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    const char *a = strstr(p, open);
    if (!a || a >= end) {
        return NULL;
    }
    a += strlen(open);
    const char *b = strchr(a, '<');
    if (!b || b > end) {
        return NULL;
    }
    size_t n = (size_t)(b - a) < len - 1 ? (size_t)(b - a) : len - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return b;
}

typedef struct {
    float lat, lon;            // fetch location
    traffic_item_t *items;     // PSRAM
    int count;
    char *buf;
    size_t len;
} parse_ctx_t;

static void handle_situation(parse_ctx_t *pc, const char *p, const char *end)
{
    traffic_item_t it = { 0 };
    char v[64];

    // Coordinates of the first point; skip anything outside the area we keep.
    if (!tag_text(p, end, "loc:latitude", v, sizeof(v))) {
        return;
    }
    it.lat = strtof(v, NULL);
    if (!tag_text(p, end, "loc:longitude", v, sizeof(v))) {
        return;
    }
    it.lon = strtof(v, NULL);
    it.dist_km = fuel_distance_km(pc->lat, pc->lon, it.lat, it.lon);
    if (it.dist_km > KEEP_KM) {
        return;
    }

    // Every record of the situation contributes; the most severe one names it.
    static const char *const KINDS[] = {
        "sit:accidentType", "sit:abnormalTrafficType", "sit:vehicleObstructionType", "sit:obstructionType",
        "sit:environmentalObstructionType", "sit:infrastructureDamageType", "sit:nonWeatherRelatedRoadConditionType",
        "sit:poorEnvironmentType", "sit:roadMaintenanceType", "sit:speedManagementType",
        "sit:generalInstructionToRoadUsersType", "sit:roadOrCarriagewayOrLaneManagementType",
    };
    const label_t *main = NULL, *mgmt = NULL;
    for (size_t k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); k++) {
        const char *q = p;
        while ((q = tag_text(q, end, KINDS[k], v, sizeof(v))) != NULL) {
            const label_t *l = label_for(v);
            if (!l) {
                continue;
            }
            bool is_mgmt = k == sizeof(KINDS) / sizeof(KINDS[0]) - 1;
            if (is_mgmt && (!mgmt || l->sev > mgmt->sev)) {
                mgmt = l;
            }
            if (!main || l->sev > main->sev) {
                main = l;
            }
        }
    }
    if (!main && tag_text(p, end, "sit:causeType", v, sizeof(v))) {
        main = label_for(v);
    }
    if (!main) {
        return;
    }
    it.sev = main->sev;
    if (tag_text(p, end, "sit:severity", v, sizeof(v)) && strcmp(v, "highest") == 0 && it.sev < TRAFFIC_MEDIUM) {
        it.sev = TRAFFIC_MEDIUM;  // the DGT flags it as important
    }
    strlcpy(it.title, main->label, sizeof(it.title));
    if (mgmt && mgmt != main && strcmp(mgmt->label, main->label) != 0) {
        strlcpy(it.detail, mgmt->label, sizeof(it.detail));
    }
    tag_text(p, end, "loc:roadName", it.road, sizeof(it.road));
    if (tag_text(p, end, "lse:kilometerPoint", v, sizeof(v))) {
        float km = strtof(v, NULL);
        snprintf(it.km, sizeof(it.km), km == floorf(km) ? "%.0f" : "%.1f", km);
    }
    tag_text(p, end, "lse:municipality", it.place, sizeof(it.place));
    const char *q = p;
    while ((q = tag_text(q, end, "loc:tpegDirection", v, sizeof(v))) != NULL) {
        if (strcmp(v, "unknown") != 0) {
            strlcpy(it.dir, direction(v), sizeof(it.dir));
            break;
        }
    }
    if (tag_text(p, end, "com:overallStartTime", v, sizeof(v))) {
        it.start = iso8601_to_utc(v);
    }
    if (tag_text(p, end, "com:overallEndTime", v, sizeof(v))) {
        it.end = iso8601_to_utc(v);
    }

    if (pc->count < MAX_ITEMS) {
        pc->items[pc->count++] = it;
    } else {
        // Full: replace the farthest if this one is closer.
        int far = 0;
        for (int i = 1; i < pc->count; i++) {
            if (pc->items[i].dist_km > pc->items[far].dist_km) {
                far = i;
            }
        }
        if (it.dist_km < pc->items[far].dist_km) {
            pc->items[far] = it;
        }
    }
}

static bool on_xml(const char *data, size_t len, void *ctx)
{
    parse_ctx_t *pc = ctx;
    static const char OPEN[] = "<sit:situation ", CLOSE[] = "</sit:situation>";
    while (len > 0) {
        size_t n = len < SIT_BUF - 1 - pc->len ? len : SIT_BUF - 1 - pc->len;
        memcpy(pc->buf + pc->len, data, n);
        pc->len += n;
        pc->buf[pc->len] = '\0';
        data += n;
        len -= n;

        char *p = pc->buf;
        for (;;) {
            char *s = strstr(p, OPEN);
            char *e = s ? strstr(s, CLOSE) : NULL;
            if (!e) {
                break;
            }
            handle_situation(pc, s, e);
            p = e + sizeof(CLOSE) - 1;
        }
        // Keep the unfinished tail (from the last "<sit:situation " or a few bytes).
        char *rest = strstr(p, OPEN);
        if (!rest) {
            size_t l = strlen(p);
            rest = p + (l > sizeof(OPEN) ? l - sizeof(OPEN) : 0);
        }
        pc->len = strlen(rest);
        if (pc->len >= SIT_BUF - 4096) {
            pc->len = 0;  // a situation bigger than the buffer: drop it
        }
        memmove(pc->buf, rest, pc->len + 1);
    }
    return true;
}

static int cmp_dist(const void *a, const void *b)
{
    float d = ((const traffic_item_t *)a)->dist_km - ((const traffic_item_t *)b)->dist_km;
    return d < 0 ? -1 : d > 0;
}

static bool fetch(const location_t *loc)
{
    parse_ctx_t *pc = heap_caps_calloc(1, sizeof(*pc), MALLOC_CAP_SPIRAM);
    if (!pc) {
        return false;
    }
    pc->lat = loc->lat;
    pc->lon = loc->lon;
    pc->items = heap_caps_calloc(MAX_ITEMS, sizeof(traffic_item_t), MALLOC_CAP_SPIRAM);
    pc->buf = heap_caps_malloc(SIT_BUF, MALLOC_CAP_SPIRAM);
    bool ok = pc->items && pc->buf && http_get_stream_gzip(FEED_URL, on_xml, pc) == ESP_OK;
    if (ok) {
        qsort(pc->items, pc->count, sizeof(traffic_item_t), cmp_dist);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_items, pc->items, sizeof(traffic_item_t) * pc->count);
        s_count = pc->count;
        s_updated = time(NULL);
        xSemaphoreGive(s_lock);
        int major = 0;
        for (int i = 0; i < pc->count; i++) {
            major += pc->items[i].sev >= TRAFFIC_MEDIUM && pc->items[i].dist_km < 50;
        }
        ESP_LOGI(TAG, "%d incidents within %d km, %d major within 50 km", pc->count, KEEP_KM, major);
    }
    free(pc->items);
    free(pc->buf);
    free(pc);
    return ok;
}

static void notify(void)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(s_listeners[i].ctx);
        }
    }
}

// Called by the network worker about once a second.
static void traffic_step(void)
{
    static bool have, tried;
    static float last_lat, last_lon;
    static TickType_t last;
    location_t loc;
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED || !location_get(&loc)) {
        return;
    }
    // Refetch every 5 min, or when the car moved far enough that the 200 km window could be
    // missing nearby incidents (only matters once a GPS moves the location).
    const bool moved = have && fuel_distance_km(loc.lat, loc.lon, last_lat, last_lon) > 50;
    const bool due = !tried || s_kick || xTaskGetTickCount() - last >= pdMS_TO_TICKS(have ? REFRESH_MS : RETRY_MS);
    if (!moved && !due) {
        return;
    }
    s_kick = false;
    tried = true;
    last = xTaskGetTickCount();
    if (fetch(&loc)) {
        have = true;
        last_lat = loc.lat;
        last_lon = loc.lon;
        notify();
    }
}

void traffic_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_items = heap_caps_calloc(MAX_ITEMS, sizeof(traffic_item_t), MALLOC_CAP_SPIRAM);
    net_worker_register_step(traffic_step);
}

void traffic_refresh(void)
{
    s_kick = true;
}

void traffic_add_listener(traffic_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            return;
        }
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i] = (listener_t){ cb, ctx };
            return;
        }
    }
}

void traffic_remove_listener(traffic_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            s_listeners[i] = (listener_t){ 0 };
        }
    }
}

bool traffic_ready(void)
{
    return s_updated != 0;
}

int traffic_get(traffic_item_t *out, int max, float max_km, traffic_sev_t min_sev, time_t *updated)
{
    location_t loc;
    bool have_loc = location_get(&loc);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        traffic_item_t it = s_items[i];
        if (have_loc) {
            it.dist_km = fuel_distance_km(loc.lat, loc.lon, it.lat, it.lon);  // follows the car
        }
        if (it.dist_km <= max_km && it.sev >= min_sev) {
            out[n++] = it;
        }
    }
    if (updated) {
        *updated = s_updated;
    }
    xSemaphoreGive(s_lock);
    qsort(out, n, sizeof(*out), cmp_dist);
    return n;
}
