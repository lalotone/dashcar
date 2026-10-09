#pragma once

#include <stdbool.h>
#include <stddef.h>

// Single source of "where is the car" for every app. Today: the city chosen in Weather, else IP
// geolocation (works even when the forecast can't be fetched). When a GPS module is added, a fresh fix will
// take precedence here, and Fuel, Traffic, Warnings, Radar... follow the car automatically.

typedef enum {
    LOCATION_NONE,
    LOCATION_IP,      // approximate (mobile networks can be tens of km off)
    LOCATION_CITY,    // chosen in Weather > Location
    LOCATION_GPS,     // reserved for the GPS module
} location_source_t;

typedef struct {
    float lat, lon;
    location_source_t source;
    char name[96];    // "Zaragoza, Aragon, ES"
} location_t;

bool location_get(location_t *out);   // false if no location yet
const char *location_source_name(location_source_t s);
