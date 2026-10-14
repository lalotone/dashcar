#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"

typedef enum {
    WIFI_MGR_IDLE,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,      // got an IP address
    WIFI_MGR_FAILED,         // gave up on the current attempt (see reason)
} wifi_mgr_state_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} wifi_mgr_ap_t;

// Callbacks run in the default event loop task: take the LVGL lock before touching UI.
typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, const char *reason, void *ctx);
typedef void (*wifi_mgr_scan_cb_t)(const wifi_mgr_ap_t *aps, int count, void *ctx);

void wifi_mgr_init(void);

// Called (from the SNTP task) each time network time is synchronised.
void wifi_mgr_set_time_sync_cb(void (*cb)(void));

void wifi_mgr_add_listener(wifi_mgr_state_cb_t cb, void *ctx);
void wifi_mgr_remove_listener(wifi_mgr_state_cb_t cb, void *ctx);

// Starts a new connection attempt. Once connected, drops are retried indefinitely.
// `persistent`: also keep retrying in the background if the first attempt fails
// (FAILED is still reported once), e.g. a phone hotspot that isn't up yet.
void wifi_mgr_connect(const char *ssid, const char *pass, bool persistent);
void wifi_mgr_disconnect(void);

// Async scan; stops any in-progress connection attempt that hasn't succeeded yet.
bool wifi_mgr_scan(wifi_mgr_scan_cb_t cb, void *ctx);

wifi_mgr_state_t wifi_mgr_state(void);
const char *wifi_mgr_ssid(void);
int8_t wifi_mgr_rssi(void);
