#pragma once

#include <stdbool.h>
#include <stddef.h>

// Single source of "where is the car" for every app: a GPS fix (up to 30 min old), else the city chosen in
// Weather, else IP geolocation (works even when the forecast can't be fetched). `name` is the forecast
// place (the nearest named place we know), not reverse-geocoded from the fix.

typedef enum {
    LOCATION_NONE,
    LOCATION_IP,      // approximate (mobile networks can be tens of km off)
    LOCATION_CITY,    // chosen in Weather > Location
    LOCATION_GPS,     // GT-U7 module (services/gps)
} location_source_t;

typedef struct {
    float lat, lon;
    location_source_t source;
    char name[96];    // "Zaragoza, Aragon, ES"
} location_t;

// GPS-following cadence: how often position-dependent work runs (re-sorting by distance, moving map
// markers, and at most this often any refetch triggered by movement). Fetches themselves happen only
// after a real move (thresholds per service), so a moving car doesn't keep the CPU and network busy.
#define LOCATION_FOLLOW_MS (2 * 60 * 1000)

bool location_get(location_t *out);   // false if no location yet
const char *location_source_name(location_source_t s);
