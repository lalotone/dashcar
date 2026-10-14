#pragma once

#include <stdbool.h>

// Firmware updates over Wi-Fi: a small HTTP server (port 80, mDNS name "dashcar.local")
//   GET  /        version / partition info
//   POST /update  raw app image (build/dashcar.bin) — see tools/ota.sh
// Only runs while Settings > Wireless updates is on. New images boot in "pending verify"
// state; ota_mark_valid_later() confirms them after a healthy uptime, otherwise the
// bootloader rolls back to the previous image.

typedef enum {
    OTA_EVT_START,      // upload accepted
    OTA_EVT_PROGRESS,   // percent valid
    OTA_EVT_DONE,       // image written and selected; restarting soon
    OTA_EVT_FAILED,     // msg says why
} ota_event_t;

// Called from the HTTP server task: take the LVGL lock before touching UI.
typedef void (*ota_cb_t)(ota_event_t ev, int percent, const char *msg);

void ota_init(ota_cb_t cb);          // starts/stops the server following Wi-Fi + the setting
void ota_apply_setting(void);        // call after changing settings_set_ota_enabled()
bool ota_server_running(void);
const char *ota_running_partition(void);
void ota_mark_valid_later(int seconds);
