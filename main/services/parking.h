#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// "Where I parked": the spot where the car was last left.
//
// TODO(GPS): there is no GPS module yet, so until a spot has been saved this returns a
// hardcoded demo spot (`demo` = true). When the GPS is fitted, call parking_save() with the
// last good fix when the car stops for a while / the ignition goes off (or on a "Park here"
// tap), and the home card + Parked screen show the real spot automatically.

typedef struct {
    double lat, lon;
    time_t saved_at;   // UTC, 0 if unknown
    bool demo;         // hardcoded placeholder (no GPS yet)
} parking_spot_t;

void parking_get(parking_spot_t *out);
void parking_save(double lat, double lon);   // TODO(GPS): call from the GPS code

// Google Maps walking directions to the spot.
void parking_walk_url(const parking_spot_t *p, char *buf, size_t len);
