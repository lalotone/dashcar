#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Live road incidents from the DGT (Spain) DATEX II feed: accidents, closures, queues,
// roadworks... Refreshed every 10 min (gzip, ~175 KB). Distances are from location_get(),
// so they follow the car once a GPS module provides the location.

typedef enum {
    TRAFFIC_LOW = 1,      // roadworks, lane restrictions, speed limits
    TRAFFIC_MEDIUM = 2,   // slow traffic, broken-down vehicle, object on the road, rockfall
    TRAFFIC_HIGH = 3,     // accident, road closed, fire, flooding, hazardous load
} traffic_sev_t;

typedef struct {
    traffic_sev_t sev;
    char title[32];       // "Accident", "Slow traffic", "Roadworks"
    char detail[32];      // secondary: "Lane closed", "Alternating traffic" ("" if none)
    char road[16];        // "A-68"
    char km[10];          // "246.5" ("" if unknown)
    char place[40];       // municipality
    char dir[12];         // "eastbound" ("" if unknown)
    float lat, lon;
    float dist_km;        // from the current location (updated by traffic_get)
    time_t start, end;    // UTC; end 0 = open-ended
} traffic_item_t;

typedef void (*traffic_cb_t)(void *ctx);   // runs in the traffic task

void traffic_init(void);
void traffic_refresh(void);
void traffic_add_listener(traffic_cb_t cb, void *ctx);
void traffic_remove_listener(traffic_cb_t cb, void *ctx);

// Incidents within max_km (sev >= min_sev), nearest first. Returns count; *updated = fetch time.
int traffic_get(traffic_item_t *out, int max, float max_km, traffic_sev_t min_sev, time_t *updated);
// False until the first successful fetch.
bool traffic_ready(void);
