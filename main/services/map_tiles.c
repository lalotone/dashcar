#include "map_tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_jpeg_dec.h"
#include "png.h"

static const char *TAG = "tiles";

#define CACHE_BASE     "/tiles"
#define MAX_TILE_BYTES (128 * 1024)

static bool s_cache_ok;
static bool s_cache_tried;

bool map_tiles_cache_init(void)
{
    if (s_cache_tried) {
        return s_cache_ok;
    }
    s_cache_tried = true;
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = CACHE_BASE,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tile cache unavailable: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "tile cache mounted: %u / %u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));

    // Drop tiles from the old CARTO basemap (now key-only: they were "API KEY REQUIRED" images).
    DIR *dir = opendir(CACHE_BASE);
    if (dir) {
        struct dirent *e;
        char path[300];
        while ((e = readdir(dir)) != NULL) {
            if (e->d_name[0] == 'c') {
                snprintf(path, sizeof(path), CACHE_BASE "/%s", e->d_name);
                remove(path);
            }
        }
        closedir(dir);
    }
    s_cache_ok = true;
    return true;
}

static bool cache_read(const char *path, char **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n > 0 && n <= MAX_TILE_BYTES) ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
    bool ok = buf && fread(buf, 1, n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    *out = buf;
    *len = n;
    return true;
}

static void cache_write(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        remove(path);  // cache full: just don't cache
    }
}

esp_err_t map_tiles_fetch(http_session_t *http, const char *url, const char *cache_name,
                          char **png, size_t *png_len)
{
    char path[48];
    bool use_cache = cache_name && s_cache_ok;
    if (use_cache) {
        snprintf(path, sizeof(path), CACHE_BASE "/%s", cache_name);
        if (cache_read(path, png, png_len)) {
            return ESP_OK;
        }
    }
    esp_err_t err = http_session_get(http, url, MAX_TILE_BYTES, png, png_len, NULL);
    if (err == ESP_OK && use_cache) {
        cache_write(path, *png, *png_len);
    }
    return err;
}

esp_err_t map_tiles_decode_rgba(const void *png, size_t png_len, uint8_t **rgba, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, png, png_len)) {
        ESP_LOGW(TAG, "png header: %s", img.message);
        return ESP_FAIL;
    }
    img.format = PNG_FORMAT_RGBA;
    uint8_t *buf = heap_caps_malloc(PNG_IMAGE_SIZE(img), MALLOC_CAP_SPIRAM);
    if (!buf) {
        png_image_free(&img);
        return ESP_ERR_NO_MEM;
    }
    if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
        ESP_LOGW(TAG, "png decode: %s", img.message);
        free(buf);
        png_image_free(&img);
        return ESP_FAIL;
    }
    *rgba = buf;
    *w = img.width;
    *h = img.height;
    return ESP_OK;
}

esp_err_t map_tiles_decode_jpeg_rgb565(const void *jpg, size_t len, uint16_t **rgb, int *w, int *h)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = len };
    jpeg_dec_header_info_t info;
    esp_err_t ret = ESP_FAIL;
    uint8_t *out = NULL;
    int out_len = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
            jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK) {
        out = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM);
        if (!out) {
            ret = ESP_ERR_NO_MEM;
        } else {
            io.outbuf = out;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                *rgb = (uint16_t *)out;
                *w = info.width;
                *h = info.height;
                out = NULL;
                ret = ESP_OK;
            }
        }
    }
    heap_caps_free(out);
    jpeg_dec_close(dec);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed");
    }
    return ret;
}
