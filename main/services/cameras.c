#include "cameras.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/http_util.h"
#include "net/wifi_mgr.h"
#include "services/map_tiles.h"   // SPIFFS mount (/tiles)
#include "services/net_worker.h"
#include "ui/ui.h"                // ui_lock: readers are on the LVGL side

static const char *TAG = "cameras";

#define FEED_URL     "https://infocar.dgt.es/datex2/dgt/PredefinedLocationsPublication/radares/content.xml"
#define CACHE_FILE   "/tiles/cameras.bin"
#define CACHE_MAGIC  0x314D4143u      // "CAM1"
#define MAX_CAMS     1200             // 737 today (90 KB reserved)
#define REFRESH_S    (7 * 24 * 3600)  // the list changes rarely
#define RETRY_MS     (30 * 60 * 1000)
#define BLOCK_BUF    8192             // one <predefinedLocation> is ~1.5-3.5 KB

typedef struct {
    uint32_t magic;
    uint32_t count;
    int64_t  updated;
} file_hdr_t;

// One fixed array (allocated at boot, reused by every download: see speedlimit.c on fragmentation).
// A download fills it while s_count is 0 (alerts pause for those few seconds, once a week).
static camera_t *s_cams;
static char *s_block;            // parse buffer (BLOCK_BUF)
static int s_count;
static time_t s_updated;
static volatile bool s_loading;

typedef struct {
    char *buf;
    size_t len;
    camera_t *out;
    int count;
} parse_ctx_t;

// Text of the first <_0:tag>...</_0:tag> in [p, end), or NULL.
static const char *tag_text(const char *p, const char *end, const char *tag, char *out, size_t len)
{
    char open[48];
    snprintf(open, sizeof(open), "<_0:%s>", tag);
    const char *s = strstr(p, open);
    if (!s || s >= end) {
        return NULL;
    }
    s += strlen(open);
    const char *e = strchr(s, '<');
    if (!e || e >= end) {
        return NULL;
    }
    size_t n = (size_t)(e - s) < len - 1 ? (size_t)(e - s) : len - 1;
    memcpy(out, s, n);
    // Don't cut a UTF-8 character ("PADRÓN") in half when truncating.
    while (n > 0 && (size_t)(e - s) > n && ((uint8_t)s[n] & 0xC0) == 0x80) {
        n--;
    }
    out[n] = '\0';
    return e;
}

static void handle_location(parse_ctx_t *pc, const char *p, const char *end)
{
    if (pc->count >= MAX_CAMS) {
        return;
    }
    camera_t c = { .km = -1 };
    static const char SECTION[] = "<_0:predefinedLocation id=\"GUID_CVM_";
    static const char FIXED[] = "<_0:predefinedLocation id=\"GUID_CABINACINEMOMETRO_";
    if (strncmp(p, SECTION, sizeof(SECTION) - 1) == 0) {
        c.kind = CAM_SECTION;
    } else if (strncmp(p, FIXED, sizeof(FIXED) - 1) == 0) {
        c.kind = CAM_FIXED;
    } else {
        return;
    }
    char v[48];
    const char *q = tag_text(p, end, "latitude", v, sizeof(v));
    if (!q) {
        return;
    }
    c.lat = strtof(v, NULL);
    if (!(q = tag_text(q, end, "longitude", v, sizeof(v)))) {
        return;
    }
    c.lon = strtof(v, NULL);
    c.lat2 = c.lat;
    c.lon2 = c.lon;
    if (c.kind == CAM_SECTION) {
        // Sections list the "to" point first, then "from".
        const char *r = tag_text(q, end, "latitude", v, sizeof(v));
        if (!r) {
            return;
        }
        c.lat2 = strtof(v, NULL);
        if (!tag_text(r, end, "longitude", v, sizeof(v))) {
            return;
        }
        c.lon2 = strtof(v, NULL);
    }
    tag_text(p, end, "roadNumber", c.road, sizeof(c.road));
    if (tag_text(p, end, "referencePointDistance", v, sizeof(v))) {
        c.km = strtof(v, NULL) / 1000;
    }
    tag_text(p, end, "directionNamed", c.toward, sizeof(c.toward));
    pc->out[pc->count++] = c;
}

static bool on_xml(const char *data, size_t len, void *ctx)
{
    parse_ctx_t *pc = ctx;
    // Records nest a second <_0:predefinedLocation xsi:type=...>, so a record runs from one
    // "<_0:predefinedLocation id=" to the next (or to the end of its set).
    static const char OPEN[] = "<_0:predefinedLocation id=", SET_END[] = "</_0:predefinedLocationSet>";
    while (len > 0) {
        size_t n = len < BLOCK_BUF - 1 - pc->len ? len : BLOCK_BUF - 1 - pc->len;
        memcpy(pc->buf + pc->len, data, n);
        pc->len += n;
        pc->buf[pc->len] = '\0';
        data += n;
        len -= n;

        char *p = pc->buf;
        for (;;) {
            char *s = strstr(p, OPEN);
            if (!s) {
                break;
            }
            char *e = strstr(s + 1, OPEN);
            char *set_end = strstr(s, SET_END);
            if (set_end && (!e || set_end < e)) {
                e = set_end;
            }
            if (!e) {
                break;
            }
            handle_location(pc, s, e);
            p = e;
        }
        char *rest = strstr(p, OPEN);
        if (!rest) {
            size_t l = strlen(p);
            rest = p + (l > sizeof(OPEN) ? l - sizeof(OPEN) : 0);
        }
        pc->len = strlen(rest);
        if (pc->len >= BLOCK_BUF - 1024) {
            pc->len = 0;  // a record bigger than the buffer: drop it
        }
        memmove(pc->buf, rest, pc->len + 1);
    }
    return true;
}

static void set_count(int count, time_t updated)
{
    if (ui_lock()) {
        s_count = count;
        s_updated = updated;
        ui_unlock();
    }
}

static bool load_file(void)
{
    if (!s_cams || !map_tiles_cache_init()) {
        return false;
    }
    FILE *f = fopen(CACHE_FILE, "rb");
    if (!f) {
        return false;
    }
    set_count(0, s_updated);
    file_hdr_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CACHE_MAGIC && h.count > 0 && h.count <= MAX_CAMS &&
              fread(s_cams, sizeof(camera_t), h.count, f) == h.count;
    fclose(f);
    if (!ok) {
        return false;
    }
    set_count((int)h.count, (time_t)h.updated);
    ESP_LOGI(TAG, "%u cameras from flash", (unsigned)h.count);
    return true;
}

static void save_file(const camera_t *cams, int count, time_t updated)
{
    if (!map_tiles_cache_init()) {
        return;
    }
    FILE *f = fopen(CACHE_FILE, "wb");
    if (!f) {
        return;
    }
    const file_hdr_t h = { .magic = CACHE_MAGIC, .count = (uint32_t)count, .updated = updated };
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(cams, sizeof(camera_t), count, f) == (size_t)count;
    fclose(f);
    if (!ok) {
        remove(CACHE_FILE);  // flash full: keep it in RAM only
    }
}

static bool download(void)
{
    if (!s_cams) {
        return false;
    }
    parse_ctx_t pc = { .buf = s_block, .out = s_cams };
    pc.buf[0] = '\0';
    set_count(0, s_updated);
    const bool ok = http_get_stream_gzip(FEED_URL, on_xml, &pc) == ESP_OK && pc.count > 0;
    if (!ok) {
        ESP_LOGW(TAG, "download failed");
        load_file();  // partly overwritten: back to the saved list
        return false;
    }
    const time_t now = time(NULL);
    save_file(s_cams, pc.count, now);
    set_count(pc.count, now);
    ESP_LOGI(TAG, "%d cameras downloaded", pc.count);
    return true;
}

static void cameras_step(void)
{
    static bool loaded;
    static TickType_t last_try;
    static bool tried;
    if (!loaded) {
        loaded = true;
        load_file();
    }
    const time_t now = time(NULL);
    const bool stale = !s_updated || (now > 1700000000 && now - s_updated > REFRESH_S);
    if (!stale || wifi_mgr_state() != WIFI_MGR_CONNECTED || now < 1700000000) {
        return;
    }
    if (tried && xTaskGetTickCount() - last_try < pdMS_TO_TICKS(RETRY_MS)) {
        return;
    }
    tried = true;
    last_try = xTaskGetTickCount();
    s_loading = true;
    download();
    s_loading = false;
}

void cameras_init(void)
{
    s_cams = heap_caps_malloc(MAX_CAMS * sizeof(camera_t), MALLOC_CAP_SPIRAM);
    s_block = heap_caps_malloc(BLOCK_BUF, MALLOC_CAP_SPIRAM);
    if (!s_cams || !s_block) {
        ESP_LOGE(TAG, "no PSRAM for the camera list");
        heap_caps_free(s_cams);
        heap_caps_free(s_block);
        s_cams = NULL;
        s_block = NULL;
    }
    net_worker_register_step(cameras_step);
}

int cameras_list(const camera_t **list)
{
    *list = s_cams;
    return s_count;
}

time_t cameras_updated(void)
{
    return s_updated;
}

bool cameras_loading(void)
{
    return s_loading;
}
