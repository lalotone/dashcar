#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// Overlanding places of interest from OpenStreetMap (Overpass, ODbL): water, campsites, huts/shelters,
// viewpoints, picnic sites, toilets. Used only while the Explore screen is open: nothing runs in the
// background and nothing stays in RAM after it closes. Each downloaded area (25 km box) is saved in flash
// (SPIFFS, at most POI_CACHE_FILES areas) so the screen works without coverage.

typedef enum {
    POI_WATER,     // drinking water, springs
    POI_CAMP,      // campsites, caravan sites
    POI_HUT,       // alpine/wilderness huts, shelters (not bus shelters)
    POI_VIEW,      // viewpoints
    POI_PICNIC,    // picnic sites
    POI_WC,        // toilets
    POI_KINDS
} poi_kind_t;

#define POI_FEE         0x01   // fee=yes
#define POI_NOT_POTABLE 0x02   // spring/water marked drinking_water=no
#define POI_OPEN_24H    0x04

typedef struct {
    float   lat, lon;
    int16_t ele;               // m, INT16_MIN unknown
    uint8_t kind;              // poi_kind_t
    uint8_t flags;
    char    name[40];          // may be empty
} poi_t;                       // 52 B

#define POI_MAX          400   // nearest kept per area (~21 KB)
#define POI_RADIUS_KM    25
#define POI_CACHE_FILES  16    // ~340 KB of flash at most

typedef struct {
    poi_t *items;              // caller-provided, POI_MAX entries
    int    count;              // sorted by distance from (lat, lon)
    float  lat, lon;           // area centre
    time_t fetched;            // UTC
} poi_area_t;

// Blocking (network worker). On success fills `out` (items must hold POI_MAX) and saves it to flash.
esp_err_t poi_fetch(float lat, float lon, poi_area_t *out);
// Nearest saved area whose centre is within `max_km`; false if none.
bool poi_cache_load_nearest(float lat, float lon, float max_km, poi_area_t *out);
const char *poi_kind_name(poi_kind_t k);
