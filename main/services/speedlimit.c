#include "speedlimit.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/fuel.h"        // fuel_distance_km
#include "services/gps.h"
#include "services/location.h"
#include "services/map_tiles.h"   // SPIFFS mount (/tiles)
#include "services/net_worker.h"
#include "ui/ui.h"                // ui_lock: matching runs on the LVGL side

static const char *TAG = "speedlimit";

#define MAJOR_M       5000
#define MINOR_M       2000
#define REFETCH_KM    1.0f        // the minor-street ring is 2 km: refetch well before leaving it
// Fixed buffers, allocated once at boot and reused by every download: allocating and shrinking
// big temporary arrays fragmented PSRAM until the Radar's two 1.1 MB buffers no longer fit.
#define MAX_WAYS      4000        // small city: 1632 roads / 11817 points; a big city ~2-3x
#define MAX_PTS       32000
#define MATCH_M       25.0f       // max distance from the road centre line
#define DIR_DEG       35.0f       // course vs. segment direction
#define CONFIRM       2           // same new limit this many times in a row before showing it
#define LOST_S        8           // no road match this long: limit unknown
#define CACHE_FILE    "/tiles/speedlimit.bin"
#define CACHE_MAGIC   0x314D4C53u // "SLM1"
#define LINE_LEN      160

static const char *const MIRRORS[] = {
    "https://overpass-api.de/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
};

typedef struct {
    float   minlat, minlon, maxlat, maxlon;
    uint32_t first;               // index into points
    uint16_t n;
    int16_t fwd, bwd;             // km/h along / against the way's node order, 0 unknown
    uint8_t implicit;
    char    ref[12];
} way_t;

typedef struct {
    float lat, lon;               // NaN = gap (node outside the query box)
} pt_t;

typedef struct {
    uint32_t magic, ways, pts;
    float    lat, lon;            // query centre
} file_hdr_t;

// Data in use: s_have/s_nways change under ui_lock; a download fills the arrays while s_have is
// false (matching pauses for those few seconds and keeps showing the last limit).
static way_t *s_ways;             // MAX_WAYS, PSRAM, allocated once
static pt_t *s_pts;               // MAX_PTS, PSRAM, allocated once
static int s_nways;
static float s_center_lat, s_center_lon;
static bool s_have;

static volatile bool s_paused;    // Radar open: buffers released, don't reallocate

// Match state (LVGL task).
static speedlimit_t s_shown;
static int s_cand_kmh, s_cand_count;
static int s_lost_s;

// ---------------------------------------------------------------------------
// Parsing: Overpass JSON is pretty-printed with one point or tag per line.

typedef struct {
    way_t *ways;
    pt_t *pts;
    int nways, npts;
    bool in_way;
    char line[LINE_LEN];
    int len;
    bool skip_line;               // the current line overflowed: ignore it
    char highway[24], maxspeed[16], fwd[16], bwd[16];
} parse_ctx_t;

// "90", "50 mph", "ES:urban", "120;100" -> km/h (0 unknown).
static int parse_speed(const char *v, bool *implicit)
{
    *implicit = false;
    if (!v[0]) {
        return 0;
    }
    if (isdigit((unsigned char)v[0])) {
        int n = atoi(v);
        return strstr(v, "mph") ? (int)lroundf(n * 1.609344f) : n;
    }
    static const struct { const char *k; int v; } ZONES[] = {
        { "ES:urban", 50 }, { "ES:rural", 90 }, { "ES:motorway", 120 }, { "ES:trunk", 120 },
        { "ES:zone30", 30 }, { "ES:zone20", 20 }, { "ES:living_street", 20 }, { "walk", 10 },
    };
    for (size_t i = 0; i < sizeof(ZONES) / sizeof(ZONES[0]); i++) {
        if (strcmp(v, ZONES[i].k) == 0) {
            *implicit = true;
            return ZONES[i].v;
        }
    }
    return 0;  // "none", "signals", "variable"...
}

static void finish_way(parse_ctx_t *pc)
{
    if (!pc->in_way) {
        return;
    }
    pc->in_way = false;
    way_t *w = &pc->ways[pc->nways];
    if (w->n < 2) {
        return;
    }
    bool imp_base, imp_f, imp_b;
    int base = parse_speed(pc->maxspeed, &imp_base);
    if (!base && strcmp(pc->highway, "motorway") == 0) {
        base = 120;  // Spanish law for motorways/autovías without a sign
        imp_base = true;
    }
    const int f = parse_speed(pc->fwd, &imp_f), b = parse_speed(pc->bwd, &imp_b);
    w->fwd = (int16_t)(f ? f : base);
    w->bwd = (int16_t)(b ? b : base);
    w->implicit = (f ? imp_f : imp_base) && (b ? imp_b : imp_base);
    // Bounding box (for a cheap prefilter while matching).
    w->minlat = w->minlon = 1e9f;
    w->maxlat = w->maxlon = -1e9f;
    for (uint32_t i = w->first; i < w->first + w->n; i++) {
        const pt_t *p = &pc->pts[i];
        if (isnan(p->lat)) {
            continue;
        }
        w->minlat = fminf(w->minlat, p->lat);
        w->maxlat = fmaxf(w->maxlat, p->lat);
        w->minlon = fminf(w->minlon, p->lon);
        w->maxlon = fmaxf(w->maxlon, p->lon);
    }
    pc->nways++;
}

// `"key": "value",` -> value into out (only for the key we ask for).
static bool tag(const char *line, const char *key, char *out, size_t len)
{
    char k[32];
    snprintf(k, sizeof(k), "\"%s\": \"", key);
    if (strncmp(line, k, strlen(k)) != 0) {
        return false;
    }
    const char *v = line + strlen(k);
    const char *e = strchr(v, '"');
    size_t n = e ? (size_t)(e - v) : 0;
    n = n < len - 1 ? n : len - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

static void on_line(parse_ctx_t *pc, char *s)
{
    while (*s == ' ') {
        s++;
    }
    if (strncmp(s, "\"type\": \"way\"", 13) == 0) {
        finish_way(pc);
        if (pc->nways >= MAX_WAYS) {
            return;
        }
        pc->in_way = true;
        way_t *w = &pc->ways[pc->nways];
        memset(w, 0, sizeof(*w));
        w->first = pc->npts;
        pc->highway[0] = pc->maxspeed[0] = pc->fwd[0] = pc->bwd[0] = '\0';
        return;
    }
    if (!pc->in_way) {
        return;
    }
    way_t *w = &pc->ways[pc->nways];
    if (s[0] == '{' && strncmp(s, "{ \"lat\": ", 9) == 0) {
        const char *lo = strstr(s, "\"lon\": ");
        if (lo && pc->npts < MAX_PTS && w->n < UINT16_MAX) {
            pc->pts[pc->npts++] = (pt_t){ strtof(s + 9, NULL), strtof(lo + 7, NULL) };
            w->n++;
        }
    } else if (strncmp(s, "null", 4) == 0) {
        if (pc->npts < MAX_PTS && w->n < UINT16_MAX) {
            pc->pts[pc->npts++] = (pt_t){ NAN, NAN };
            w->n++;
        }
    } else if (s[0] == '"') {
        if (!tag(s, "highway", pc->highway, sizeof(pc->highway)) &&
            !tag(s, "maxspeed", pc->maxspeed, sizeof(pc->maxspeed)) &&
            !tag(s, "maxspeed:forward", pc->fwd, sizeof(pc->fwd)) &&
            !tag(s, "maxspeed:backward", pc->bwd, sizeof(pc->bwd))) {
            tag(s, "ref", w->ref, sizeof(w->ref));
        }
    }
}

static bool on_data(const char *data, size_t len, void *ctx)
{
    parse_ctx_t *pc = ctx;
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n') {
            if (!pc->skip_line) {
                pc->line[pc->len] = '\0';
                on_line(pc, pc->line);
            }
            pc->len = 0;
            pc->skip_line = false;
        } else if (pc->len < LINE_LEN - 1) {
            pc->line[pc->len++] = c;
        } else {
            pc->skip_line = true;  // long name/tag lines: not needed
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Storage

static void publish(int nways, float lat, float lon)
{
    if (ui_lock()) {
        s_nways = nways;
        s_center_lat = lat;
        s_center_lon = lon;
        s_have = nways > 0;
        ui_unlock();
    }
}

static void unpublish(void)
{
    if (ui_lock()) {
        s_have = false;  // matching stops reading the arrays
        s_nways = 0;
        ui_unlock();
    }
}

static void save_file(const way_t *ways, int nways, const pt_t *pts, int npts, float lat, float lon)
{
    if (!map_tiles_cache_init()) {
        return;
    }
    FILE *f = fopen(CACHE_FILE, "wb");
    if (!f) {
        return;
    }
    const file_hdr_t h = { CACHE_MAGIC, (uint32_t)nways, (uint32_t)npts, lat, lon };
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(ways, sizeof(way_t), nways, f) == (size_t)nways &&
              fwrite(pts, sizeof(pt_t), npts, f) == (size_t)npts;
    fclose(f);
    if (!ok) {
        remove(CACHE_FILE);
    }
}

static bool load_file(void)
{
    if (!s_ways || !map_tiles_cache_init()) {
        return false;
    }
    FILE *f = fopen(CACHE_FILE, "rb");
    if (!f) {
        return false;
    }
    unpublish();
    file_hdr_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CACHE_MAGIC && h.ways && h.ways <= MAX_WAYS &&
              h.pts && h.pts <= MAX_PTS && fread(s_ways, sizeof(way_t), h.ways, f) == h.ways &&
              fread(s_pts, sizeof(pt_t), h.pts, f) == h.pts;
    fclose(f);
    if (!ok) {
        return false;
    }
    publish((int)h.ways, h.lat, h.lon);
    ESP_LOGI(TAG, "%u roads from flash", (unsigned)h.ways);
    return true;
}

static bool download(float lat, float lon, int mirror)
{
    // Box for "out geom(...)": geometry is clipped to it (nodes outside become null).
    const float dlat = MAJOR_M / 111000.0f, dlon = dlat / cosf(lat * (float)M_PI / 180);
    EXT_RAM_BSS_ATTR static char q[600], enc[1100], url[1300];  // network worker only (its stack is tight internal RAM)
    // Bounding boxes, not around(): much cheaper for the (often overloaded) public servers. Only
    // roads with a limit, plus motorways (120 by law when untagged).
    const float mlat = MINOR_M / 111000.0f, mlon = mlat / cosf(lat * (float)M_PI / 180);
    snprintf(q, sizeof(q),
             "[out:json][timeout:40][bbox:%.4f,%.4f,%.4f,%.4f];("
             "way[highway~\"^(trunk|primary|secondary|tertiary)(_link)?$\"][maxspeed];"
             "way[highway~\"^motorway(_link)?$\"];"
             "way(%.4f,%.4f,%.4f,%.4f)[highway~\"^(unclassified|residential|living_street)$\"][maxspeed];"
             ");out tags geom;",
             lat - dlat, lon - dlon, lat + dlat, lon + dlon, lat - mlat, lon - mlon, lat + mlat, lon + mlon);
    http_url_encode(q, enc, sizeof(enc));
    snprintf(url, sizeof(url), "%s?data=%s", MIRRORS[mirror], enc);

    EXT_RAM_BSS_ATTR static parse_ctx_t pc_store;   // ~250 B, PSRAM; static: no allocation per download
    parse_ctx_t *pc = &pc_store;
    if (!s_ways) {
        return false;
    }
    memset(pc, 0, sizeof(*pc));
    pc->ways = s_ways;
    pc->pts = s_pts;
    unpublish();
    bool ok = http_get_stream_gzip_timeout(url, on_data, pc, 45000) == ESP_OK;
    if (ok) {
        finish_way(pc);
        ok = pc->nways > 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "download failed (%s)", MIRRORS[mirror]);
        load_file();  // the arrays were partly overwritten: back to the last saved area
        return false;
    }
    save_file(s_ways, pc->nways, s_pts, pc->npts, lat, lon);
    publish(pc->nways, lat, lon);
    ESP_LOGI(TAG, "%d roads, %d points around %.4f,%.4f", pc->nways, pc->npts, lat, lon);
    return true;
}

// Network worker, ~1/s: refetch when the GPS has taken us > REFETCH_KM from the loaded area,
// at most every LOCATION_FOLLOW_MS (Overpass is a shared free service).
static bool alloc_buffers(void)
{
    if (s_ways) {
        return true;
    }
    s_ways = heap_caps_malloc(MAX_WAYS * sizeof(way_t), MALLOC_CAP_SPIRAM);
    s_pts = heap_caps_malloc(MAX_PTS * sizeof(pt_t), MALLOC_CAP_SPIRAM);
    if (!s_ways || !s_pts) {
        ESP_LOGW(TAG, "no PSRAM for the road buffers");
        heap_caps_free(s_ways);
        heap_caps_free(s_pts);
        s_ways = NULL;
        s_pts = NULL;
        return false;
    }
    return true;
}

static void speedlimit_step(void)
{
    static bool loaded;
    static TickType_t last_try;
    static bool tried;
    static int mirror;
    if (s_paused) {
        return;
    }
    if (!s_ways) {
        loaded = false;  // released for the Radar: back from flash
        if (!alloc_buffers()) {
            return;
        }
    }
    if (!loaded) {
        loaded = true;
        load_file();
    }
    location_t loc;
    if (wifi_mgr_state() != WIFI_MGR_CONNECTED || !location_get(&loc) || loc.source != LOCATION_GPS) {
        return;
    }
    if (tried && xTaskGetTickCount() - last_try < pdMS_TO_TICKS(LOCATION_FOLLOW_MS)) {
        return;
    }
    if (s_have && fuel_distance_km(loc.lat, loc.lon, s_center_lat, s_center_lon) < REFETCH_KM) {
        return;
    }
    tried = true;
    last_try = xTaskGetTickCount();
    if (!download(loc.lat, loc.lon, mirror)) {
        mirror = (mirror + 1) % (int)(sizeof(MIRRORS) / sizeof(MIRRORS[0]));  // busy (504): next one next time
    }
}

void speedlimit_init(void)
{
    alloc_buffers();  // early, while PSRAM is unfragmented
    net_worker_register_step(speedlimit_step);
}

void speedlimit_pause(void)
{
    s_paused = true;
}

void speedlimit_release(void)
{
    if (!s_ways) {
        return;
    }
    if (ui_lock()) {
        s_have = false;
        s_nways = 0;
        memset(&s_shown, 0, sizeof(s_shown));  // no stale sign while the Radar is open
        ui_unlock();
    }
    heap_caps_free(s_ways);
    heap_caps_free(s_pts);
    s_ways = NULL;
    s_pts = NULL;
    ESP_LOGI(TAG, "buffers released for the Radar");
}

void speedlimit_resume(void)
{
    s_paused = false;  // the next step reallocates and reloads the area from flash
}

// ---------------------------------------------------------------------------
// Matching (LVGL task)

static float angle_diff(float a, float b)
{
    return fabsf(fmodf(a - b + 540, 360) - 180);
}

void speedlimit_update(void)
{
    gps_fix_t f;
    int sats;
    const bool live = gps_get(&f) && gps_state(&sats) == GPS_FIX;
    if (!s_have) {
        return;  // a download is replacing the roads: keep showing the last limit
    }
    if (!live) {
        if (++s_lost_s > LOST_S) {
            memset(&s_shown, 0, sizeof(s_shown));
        }
        return;
    }
    if (f.speed_kmh < GPS_MIN_KMH) {
        return;  // stopped: no heading; keep what we had
    }
    // Local metric projection around the car.
    const float lat0 = (float)f.lat, lon0 = (float)f.lon;
    const float ky = 110540.0f, kx = 111320.0f * cosf(lat0 * (float)M_PI / 180);
    const float pad = 0.0005f;  // ~50 m
    const way_t *best = NULL;
    bool best_fwd = true;
    float best_d = MATCH_M;
    for (int i = 0; i < s_nways; i++) {
        const way_t *w = &s_ways[i];
        if (lat0 < w->minlat - pad || lat0 > w->maxlat + pad || lon0 < w->minlon - pad || lon0 > w->maxlon + pad) {
            continue;
        }
        for (uint32_t j = w->first; j + 1 < w->first + w->n; j++) {
            const pt_t *a = &s_pts[j], *b = &s_pts[j + 1];
            if (isnan(a->lat) || isnan(b->lat)) {
                continue;
            }
            const float ax = (a->lon - lon0) * kx, ay = (a->lat - lat0) * ky;
            const float bx = (b->lon - lon0) * kx, by = (b->lat - lat0) * ky;
            const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
            if (l2 < 1) {
                continue;
            }
            float t = -(ax * dx + ay * dy) / l2;  // projection of the car (origin) on the segment
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float px = ax + t * dx, py = ay + t * dy, d = sqrtf(px * px + py * py);
            if (d >= best_d) {
                continue;
            }
            const float seg = fmodf(atan2f(dx, dy) * 180 / (float)M_PI + 360, 360);  // 0 = north
            const float diff = angle_diff(seg, f.course);
            if (diff < DIR_DEG || diff > 180 - DIR_DEG) {
                best = w;
                best_fwd = diff < 90;
                best_d = d;
            }
        }
    }
    if (!best) {
        if (++s_lost_s > LOST_S) {
            memset(&s_shown, 0, sizeof(s_shown));
        }
        return;
    }
    s_lost_s = 0;
    const int kmh = best_fwd ? best->fwd : best->bwd;
    if (kmh != s_shown.kmh) {
        // Confirm a change (junctions, parallel roads) before showing it.
        s_cand_count = kmh == s_cand_kmh ? s_cand_count + 1 : 1;
        s_cand_kmh = kmh;
        if (s_cand_count < CONFIRM && s_shown.kmh) {
            return;
        }
    }
    s_shown.kmh = kmh;
    s_shown.implicit = best->implicit;
    strlcpy(s_shown.ref, best->ref, sizeof(s_shown.ref));
}

void speedlimit_get(speedlimit_t *out)
{
    *out = s_shown;
}

int speedlimit_road_count(void)
{
    return s_nways;
}
