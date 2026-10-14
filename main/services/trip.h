#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Trip computer, fed by the GPS (services/gps calls trip_on_fix() for every position fix).
// A trip starts when the car first moves and ends after TRIP_GAP_S without moving (the board is
// powered with the ignition, so a long pause is also a power-off); a new one can be started by hand.
// Persisted in NVS every LOCATION_FOLLOW_MS while it changes and at every stop.

#define TRIP_GAP_S (2 * 3600)

typedef struct {
    float    km;
    uint32_t moving_s;     // driving time (speed >= 5 km/h)
    uint32_t stopped_s;    // stopped time since the trip started
    float    max_kmh;
    time_t   start;        // UTC, 0 = no trip yet
    time_t   last;         // UTC of the last update
} trip_t;

typedef struct {
    trip_t   current;
    trip_t   previous;
    float    today_km;
    uint32_t today_moving_s;
    float    month_km;
    double   total_km;     // GPS odometer since the first fix
} trip_info_t;

void trip_init(void);
// From the GPS (esp_timer context): one fix, roughly once a second.
void trip_on_fix(double lat, double lon, float kmh);
void trip_get(trip_info_t *out);
void trip_reset(void);    // current -> previous, start a new trip at the next movement
