#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// "Where I parked": the spot where the car was last left.
//
// services/gps calls parking_save() when the car stops for a while after driving. Until the first
// save this returns a hardcoded demo spot (`demo` = true).

typedef struct {
    double lat, lon;
    time_t saved_at;   // UTC, 0 if unknown
    bool demo;         // hardcoded placeholder (nothing saved yet)
} parking_spot_t;

void parking_get(parking_spot_t *out);
void parking_save(double lat, double lon);

// Google Maps walking directions to the spot.
void parking_walk_url(const parking_spot_t *p, char *buf, size_t len);
