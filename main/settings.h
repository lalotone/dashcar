#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_SSID_MAX     33  // 32 chars + NUL (802.11 limit)
#define SETTINGS_PASS_MAX     65  // 64 chars + NUL (WPA2 limit)
#define SETTINGS_PLACE_MAX    96

typedef struct {
    bool  valid;     // false => locate by IP
    float lat;
    float lon;
    char  name[SETTINGS_PLACE_MAX];
} settings_location_t;

void settings_init(void);

bool settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX]);
void settings_set_wifi(const char *ssid, const char *pass);
void settings_clear_wifi(void);

void settings_get_location(settings_location_t *loc);
void settings_set_location(const settings_location_t *loc);  // NULL => back to IP location

bool settings_get_imperial(void);
void settings_set_imperial(bool imperial);

// Fuel app: product ID from the Ministerio API (4 = Gasóleo A, 1 = Gasolina 95 E5...)
// and search radius in km.
int  settings_get_fuel_product(void);
void settings_set_fuel_product(int id);
int  settings_get_fuel_radius(void);
void settings_set_fuel_radius(int km);

// Night mode: dark overlay after sunset.
typedef enum {
    NIGHT_MODE_AUTO = 0,
    NIGHT_MODE_ON = 1,
    NIGHT_MODE_OFF = 2,
} night_mode_t;
night_mode_t settings_get_night_mode(void);
void settings_set_night_mode(night_mode_t mode);

// Accept firmware uploads over Wi-Fi (tools/ota.sh). Default on.
bool settings_get_ota_enabled(void);
void settings_set_ota_enabled(bool on);

// Last known UTC offset (s) of the forecast location: lets the clock show local time
// from the RTC at boot, before the first forecast arrives.
int32_t settings_get_utc_offset(void);
void settings_set_utc_offset(int32_t offset);

// Last parking spot ("Where I parked"); false if none saved.
#include <time.h>
bool settings_get_parking(double *lat, double *lon, time_t *saved_at);
void settings_set_parking(double lat, double lon, time_t saved_at);

// Opaque cached data (e.g. the last good forecast). get() fails unless the stored size is exactly `len`,
// so a struct that changed between firmware versions is simply ignored.
bool settings_get_cache(const char *key, void *buf, size_t len);
void settings_set_cache(const char *key, const void *buf, size_t len);
