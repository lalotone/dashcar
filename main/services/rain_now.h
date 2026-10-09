#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// "Rain around me right now": a small map (dark basemap + latest RainViewer radar frame)
// centred on location_get(), kept up to date in the background so the home screen can show
// it instantly. Base rebuilt when the location moves > 3 km; radar refreshed every 10 min.
// Basemap zoom 9 (~100 x 80 km, i.e. about 50 km around the car); radar is RainViewer's
// zoom 7 (free-tier max) shown 4x. Runs as a step of the network worker.

#define RAIN_NOW_W    440
#define RAIN_NOW_H    340
#define RAIN_NOW_ZOOM 9

typedef void (*rain_now_cb_t)(void *ctx);   // runs in the rain_now task

void rain_now_init(void);
void rain_now_add_listener(rain_now_cb_t cb, void *ctx);
void rain_now_remove_listener(rain_now_cb_t cb, void *ctx);

// Current image (RGB565, RAIN_NOW_W x RAIN_NOW_H) or NULL if not ready. The buffer stays
// valid until the next listener notification; read it under the LVGL lock.
const uint16_t *rain_now_image(time_t *radar_time);
// World-pixel origin (zoom RAIN_NOW_ZOOM) of the image, to place markers on it.
void rain_now_origin(int32_t *ox, int32_t *oy);

// The Radar app needs the PSRAM: pause() stops updates, release() frees the ~1 MB of
// buffers and must run on the network worker (the Radar loader job calls it first, so it
// can't overlap a rain-map update). resume() rebuilds them when the Radar closes.
void rain_now_pause(void);
void rain_now_release(void);
void rain_now_resume(void);
