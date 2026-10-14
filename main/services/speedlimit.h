#pragma once

#include <stdbool.h>
#include <stdint.h>

// Speed limit of the road the car is on, from OpenStreetMap `maxspeed` (Overpass API, ODbL).
// The network worker downloads the roads around the GPS position (major roads 5 km, tagged
// streets 2 km) when the car has moved > 1 km, at most every LOCATION_FOLLOW_MS, and keeps the
// last area in flash. Matching runs on the LVGL task: call speedlimit_update() once a second.

typedef struct {
    int  kmh;          // 0 = unknown (no match, untagged road, "variable"...)
    char ref[12];      // road ref ("A-2") or empty
    bool implicit;     // from the road class (untagged motorway = 120), not a sign
} speedlimit_t;

void speedlimit_init(void);
// LVGL task (or under ui_lock), ~1 s: match the current GPS fix to a road.
void speedlimit_update(void);
// Last result of speedlimit_update() (LVGL task / under ui_lock).
void speedlimit_get(speedlimit_t *out);
int speedlimit_road_count(void);   // roads loaded (0 = no data)
// The Radar needs ~3 MB of PSRAM: like the home rain map (rain_now_*), the ~400 KB road buffers are
// freed while it's open. pause() on open; release() from the Radar's loader job (network worker);
// resume() when it closes (the area comes back from flash on the next step).
void speedlimit_pause(void);
void speedlimit_release(void);
void speedlimit_resume(void);
