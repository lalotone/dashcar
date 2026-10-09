#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// Spanish fuel prices from the Ministerio's open REST API ("Precios Carburantes"),
// updated every 30 min. No key needed.

typedef struct {
    int id;              // IDProducto
    const char *label;   // short UI label
    const char *name;    // official name
} fuel_product_t;

extern const fuel_product_t FUEL_PRODUCTS[];
extern const int FUEL_PRODUCT_COUNT;
const fuel_product_t *fuel_product(int id);

typedef struct {
    char  brand[40];     // "Rótulo", as on the sign (uppercase)
    char  address[72];   // title-cased
    char  town[40];
    char  hours[64];
    float lat, lon;
    float price;         // €/L (€/kg for hydrogen)
    float dist_km;
    bool  h24;
} fuel_station_t;

typedef struct {
    int product;
    float lat, lon;
    char updated[24];    // "08/10/2026 14:38:24" (Ministerio timestamp, Spain local time)
    time_t fetched_at;
    int count;
    fuel_station_t *stations;  // PSRAM, sorted by distance, within FUEL_MAX_KM
} fuel_result_t;

#define FUEL_MAX_KM 50

// Blocking (network): call from a worker task. On success *out owns `stations`.
esp_err_t fuel_fetch(int product, float lat, float lon, fuel_result_t *out);
void fuel_result_free(fuel_result_t *r);

float fuel_distance_km(float lat1, float lon1, float lat2, float lon2);
