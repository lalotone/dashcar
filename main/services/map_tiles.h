#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "net/http_util.h"

// Web Mercator "world pixel" coordinates for 256px tiles at zoom z.
static inline double mt_lon_to_px(double lon, int z)
{
    return (lon + 180.0) / 360.0 * 256.0 * (double)(1 << z);
}

static inline double mt_lat_to_px(double lat, int z)
{
    double s = sin(lat * M_PI / 180.0);
    return (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * 256.0 * (double)(1 << z);
}

static inline double mt_px_to_lon(double x, int z)
{
    return x / (256.0 * (double)(1 << z)) * 360.0 - 180.0;
}

static inline double mt_px_to_lat(double y, int z)
{
    double n = M_PI - 2.0 * M_PI * y / (256.0 * (double)(1 << z));
    return atan(sinh(n)) * 180.0 / M_PI;
}

// Mounts the "storage" partition (SPIFFS) used as tile cache. Formats it on first use,
// which can take several seconds. Safe to call repeatedly.
bool map_tiles_cache_init(void);

// Fetches a tile (PNG/JPEG). If `cache_name` is given (short, e.g. "b8_126_94"), the tile is
// served from / stored in the flash cache. Caller frees *png.
esp_err_t map_tiles_fetch(http_session_t *http, const char *url, const char *cache_name,
                          char **png, size_t *png_len);

// Decodes a baseline JPEG into a newly allocated RGB565 (LVGL native) buffer in PSRAM.
esp_err_t map_tiles_decode_jpeg_rgb565(const void *jpg, size_t len, uint16_t **rgb, int *w, int *h);

// Decodes a PNG into a newly allocated RGBA8888 buffer in PSRAM. Caller frees *rgba.
esp_err_t map_tiles_decode_rgba(const void *png, size_t png_len, uint8_t **rgba, int *w, int *h);
