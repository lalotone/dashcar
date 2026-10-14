#include "poi.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "net/http_util.h"
#include "services/fuel.h"        // fuel_distance_km
#include "services/map_tiles.h"   // SPIFFS mount (/tiles)

static const char *TAG = "poi";

#define CACHE_DIR    "/tiles"
#define CACHE_PREFIX "poi_"
#define CACHE_MAGIC  0x31494F50u  // "POI1"
#define LINE_LEN     160

static const char *const MIRRORS[] = {
    "https://overpass-api.de/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
};

typedef struct {
    uint32_t magic, count;
    float    lat, lon;
    int64_t  fetched;
} file_hdr_t;

const char *poi_kind_name(poi_kind_t k)
{
    static const char *const N[POI_KINDS] = { "Water", "Campsite", "Hut / shelter", "Viewpoint", "Picnic site",
                                              "Toilets" };
    return k < POI_KINDS ? N[k] : "";
}

// ---------------------------------------------------------------------------
// Parsing: Overpass JSON, one value per line (as in services/speedlimit.c). Elements are kept only if
// they're a kind we want and not private; the POI_MAX nearest to the centre win.

typedef struct {
    poi_area_t *out;
    bool in_el;
    poi_t cur;
    bool have_pos;
    char amenity[24], tourism[24], natural[16], shelter[24], access[16], fee[8], water[8], hours[16];
    char line[LINE_LEN];
    int len;
    bool skip_line;
} parse_ctx_t;

static void tag(const char *line, const char *key, char *out, size_t len)
{
    char k[32];
    snprintf(k, sizeof(k), "\"%s\": \"", key);
    const size_t kl = strlen(k);
    if (strncmp(line, k, kl) != 0) {
        return;
    }
    const char *v = line + kl, *e = strchr(v, '"');
    size_t n = e ? (size_t)(e - v) : 0;
    n = n < len - 1 ? n : len - 1;
    // Don't cut a UTF-8 character in half.
    while (n > 0 && e && (size_t)(e - v) > n && ((uint8_t)v[n] & 0xC0) == 0x80) {
        n--;
    }
    memcpy(out, v, n);
    out[n] = '\0';
}

static int kind_of(const parse_ctx_t *pc)
{
    if (!strcmp(pc->amenity, "drinking_water") || !strcmp(pc->natural, "spring")) return POI_WATER;
    if (!strcmp(pc->tourism, "camp_site") || !strcmp(pc->tourism, "caravan_site")) return POI_CAMP;
    if (!strcmp(pc->tourism, "alpine_hut") || !strcmp(pc->tourism, "wilderness_hut")) return POI_HUT;
    if (!strcmp(pc->amenity, "shelter") && strcmp(pc->shelter, "public_transport") != 0) return POI_HUT;
    if (!strcmp(pc->tourism, "viewpoint")) return POI_VIEW;
    if (!strcmp(pc->tourism, "picnic_site")) return POI_PICNIC;
    if (!strcmp(pc->amenity, "toilets")) return POI_WC;
    return -1;
}

static void finish_el(parse_ctx_t *pc)
{
    if (!pc->in_el) {
        return;
    }
    pc->in_el = false;
    const int k = kind_of(pc);
    if (k < 0 || !pc->have_pos || !strcmp(pc->access, "private") || !strcmp(pc->access, "no")) {
        return;
    }
    poi_t *p = &pc->cur;
    p->kind = (uint8_t)k;
    p->flags = (!strcmp(pc->fee, "yes") ? POI_FEE : 0) | (!strcmp(pc->water, "no") ? POI_NOT_POTABLE : 0) |
               (!strcmp(pc->hours, "24/7") ? POI_OPEN_24H : 0);
    poi_area_t *a = pc->out;
    if (a->count < POI_MAX) {
        a->items[a->count++] = *p;
        return;
    }
    // Full: replace the farthest if this one is nearer. A flat-earth squared distance is plenty
    // within 25 km and avoids ~300k trig calls for a city's worth of fountains.
    const float kx = cosf(a->lat * (float)M_PI / 180);
    int far = 0;
    float far_d = -1;
    for (int i = 0; i < a->count; i++) {
        const float dx = (a->items[i].lon - a->lon) * kx, dy = a->items[i].lat - a->lat, d = dx * dx + dy * dy;
        if (d > far_d) {
            far_d = d;
            far = i;
        }
    }
    const float dx = (p->lon - a->lon) * kx, dy = p->lat - a->lat;
    if (dx * dx + dy * dy < far_d) {
        a->items[far] = *p;
    }
}

static void on_line(parse_ctx_t *pc, char *s)
{
    while (*s == ' ') {
        s++;
    }
    if (!strncmp(s, "\"type\": \"node\"", 14) || !strncmp(s, "\"type\": \"way\"", 13) ||
        !strncmp(s, "\"type\": \"relation\"", 18)) {
        finish_el(pc);
        pc->in_el = true;
        pc->have_pos = false;
        memset(&pc->cur, 0, sizeof(pc->cur));
        pc->cur.ele = INT16_MIN;
        pc->amenity[0] = pc->tourism[0] = pc->natural[0] = pc->shelter[0] = '\0';
        pc->access[0] = pc->fee[0] = pc->water[0] = pc->hours[0] = '\0';
        return;
    }
    if (!pc->in_el) {
        return;
    }
    if (!strncmp(s, "\"lat\": ", 7)) {
        pc->cur.lat = strtof(s + 7, NULL);
    } else if (!strncmp(s, "\"lon\": ", 7)) {
        pc->cur.lon = strtof(s + 7, NULL);
        pc->have_pos = true;
    } else if (s[0] == '"') {
        char ele[16] = "";
        tag(s, "name", pc->cur.name, sizeof(pc->cur.name));
        tag(s, "amenity", pc->amenity, sizeof(pc->amenity));
        tag(s, "tourism", pc->tourism, sizeof(pc->tourism));
        tag(s, "natural", pc->natural, sizeof(pc->natural));
        tag(s, "shelter_type", pc->shelter, sizeof(pc->shelter));
        tag(s, "access", pc->access, sizeof(pc->access));
        tag(s, "fee", pc->fee, sizeof(pc->fee));
        tag(s, "drinking_water", pc->water, sizeof(pc->water));
        tag(s, "opening_hours", pc->hours, sizeof(pc->hours));
        tag(s, "ele", ele, sizeof(ele));
        if (ele[0]) {
            pc->cur.ele = (int16_t)atoi(ele);
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
            pc->skip_line = true;  // long descriptions/notes: not needed
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Flash cache: one file per area, the oldest dropped beyond POI_CACHE_FILES.

static float s_sort_lat, s_sort_lon;

static int cmp_dist(const void *a, const void *b)
{
    const poi_t *x = a, *y = b;
    const float d = fuel_distance_km(s_sort_lat, s_sort_lon, x->lat, x->lon) -
                    fuel_distance_km(s_sort_lat, s_sort_lon, y->lat, y->lon);
    return d < 0 ? -1 : d > 0;
}

static void sort_area(poi_area_t *a)
{
    s_sort_lat = a->lat;   // network worker only
    s_sort_lon = a->lon;
    qsort(a->items, a->count, sizeof(poi_t), cmp_dist);
}

static bool read_hdr(const char *path, file_hdr_t *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const bool ok = fread(h, sizeof(*h), 1, f) == 1 && h->magic == CACHE_MAGIC && h->count <= POI_MAX;
    fclose(f);
    return ok;
}

static void prune_cache(void)
{
    for (;;) {
        DIR *dir = opendir(CACHE_DIR);
        if (!dir) {
            return;
        }
        int n = 0;
        int64_t oldest = INT64_MAX;
        char oldest_path[300] = "", path[300];
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (strncmp(e->d_name, CACHE_PREFIX, strlen(CACHE_PREFIX)) != 0) {
                continue;
            }
            snprintf(path, sizeof(path), CACHE_DIR "/%s", e->d_name);
            file_hdr_t h;
            const int64_t t = read_hdr(path, &h) ? h.fetched : 0;
            n++;
            if (t < oldest) {
                oldest = t;
                strlcpy(oldest_path, path, sizeof(oldest_path));
            }
        }
        closedir(dir);
        if (n <= POI_CACHE_FILES || !oldest_path[0]) {
            return;
        }
        remove(oldest_path);
    }
}

static void save_area(const poi_area_t *a)
{
    char path[64];
    // One file per ~5 km cell, so a nearby re-download replaces instead of piling up.
    snprintf(path, sizeof(path), CACHE_DIR "/" CACHE_PREFIX "%d_%d", (int)lroundf(a->lat * 20), (int)lroundf(a->lon * 20));
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    const file_hdr_t h = { CACHE_MAGIC, (uint32_t)a->count, a->lat, a->lon, a->fetched };
    const bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(a->items, sizeof(poi_t), a->count, f) == (size_t)a->count;
    fclose(f);
    if (!ok) {
        remove(path);
        return;
    }
    prune_cache();
}

bool poi_cache_load_nearest(float lat, float lon, float max_km, poi_area_t *out)
{
    if (!map_tiles_cache_init()) {
        return false;
    }
    DIR *dir = opendir(CACHE_DIR);
    if (!dir) {
        return false;
    }
    char best[300] = "", path[300];
    float best_d = max_km;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, CACHE_PREFIX, strlen(CACHE_PREFIX)) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), CACHE_DIR "/%s", e->d_name);
        file_hdr_t h;
        if (!read_hdr(path, &h)) {
            continue;
        }
        const float d = fuel_distance_km(lat, lon, h.lat, h.lon);
        if (d < best_d) {
            best_d = d;
            strlcpy(best, path, sizeof(best));
        }
    }
    closedir(dir);
    if (!best[0]) {
        return false;
    }
    FILE *f = fopen(best, "rb");
    if (!f) {
        return false;
    }
    file_hdr_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CACHE_MAGIC && h.count <= POI_MAX &&
              fread(out->items, sizeof(poi_t), h.count, f) == h.count;
    fclose(f);
    if (ok) {
        out->count = (int)h.count;
        out->lat = h.lat;
        out->lon = h.lon;
        out->fetched = (time_t)h.fetched;
    }
    return ok;
}

// ---------------------------------------------------------------------------

esp_err_t poi_fetch(float lat, float lon, poi_area_t *out)
{
    static int mirror;   // network worker only: start with the one that worked last
    const float d = POI_RADIUS_KM / 111.0f, dl = d / cosf(lat * (float)M_PI / 180);
    // In PSRAM, not on the network worker's stack (internal RAM, ~3.3 KB spare in the soak).
    EXT_RAM_BSS_ATTR static char q[520], enc[1000], url[1100];
    snprintf(q, sizeof(q),
             "[out:json][timeout:40][bbox:%.4f,%.4f,%.4f,%.4f];("
             "nwr[amenity=drinking_water];nwr[natural=spring];"
             "nwr[tourism~\"^(camp_site|caravan_site|alpine_hut|wilderness_hut|viewpoint|picnic_site)$\"];"
             "nwr[amenity~\"^(shelter|toilets)$\"];"
             ");out center tags qt;",
             lat - d, lon - dl, lat + d, lon + dl);
    http_url_encode(q, enc, sizeof(enc));

    EXT_RAM_BSS_ATTR static parse_ctx_t pc;   // ~400 B; network worker only
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&pc, 0, sizeof(pc));
        out->count = 0;
        out->lat = lat;
        out->lon = lon;
        pc.out = out;
        snprintf(url, sizeof(url), "%s?data=%s", MIRRORS[mirror], enc);
        if (http_get_stream_gzip_timeout(url, on_data, &pc, 45000) == ESP_OK) {
            finish_el(&pc);
            out->fetched = time(NULL);
            sort_area(out);
            save_area(out);
            ESP_LOGI(TAG, "%d places around %.3f,%.3f (%s)", out->count, lat, lon, MIRRORS[mirror]);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "download failed (%s)", MIRRORS[mirror]);
        mirror = (mirror + 1) % (int)(sizeof(MIRRORS) / sizeof(MIRRORS[0]));  // busy (504): try the other
    }
    out->count = 0;
    return ESP_FAIL;
}
