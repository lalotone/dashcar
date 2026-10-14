#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Fixed speed cameras and average-speed sections on Spanish roads (DGT NAP, DATEX II
// "PredefinedLocationsPublication/radares", CC BY; Catalonia and the Basque Country are not
// included). Downloaded once a week and kept in flash (SPIFFS), so alerts work without Wi-Fi.
// The data has no speed limits and gives directions only relative to the road's kilometre posts.

typedef enum {
    CAM_FIXED,      // single point
    CAM_SECTION,    // average speed between two points
} cam_kind_t;

typedef struct {
    float   lat, lon;       // camera / one end of the section
    float   lat2, lon2;     // other end (sections)
    float   km;             // kilometre post (-1 unknown)
    uint8_t kind;           // cam_kind_t
    char    road[12];       // "A-2"
    char    toward[24];     // "ZARAGOZA" (may be empty)
} camera_t;

void cameras_init(void);
// Call with the LVGL lock held (ui_lock), or from the LVGL task: a refresh swaps the array under
// that lock. Returns the count and sets *list (valid while the lock is held).
int cameras_list(const camera_t **list);
time_t cameras_updated(void);     // UTC of the data in use, 0 if none
bool cameras_loading(void);
