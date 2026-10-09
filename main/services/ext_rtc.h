#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"

// PCF85063A real-time clock on the shared I2C bus. Keeps UTC.
// Without a backup cell on the board's RTC connector it only survives resets, not power-off.

typedef enum {
    EXT_RTC_ABSENT,     // no answer on I2C
    EXT_RTC_INVALID,    // oscillator stopped / never set (time lost)
    EXT_RTC_VALID,      // system time was set from the RTC at boot
} ext_rtc_state_t;

// Reads the RTC and, if valid, sets the system clock from it. Call once, early.
ext_rtc_state_t ext_rtc_init(i2c_master_bus_handle_t bus);
// Writes the current system time (e.g. after an SNTP sync).
void ext_rtc_save_now(void);
ext_rtc_state_t ext_rtc_state(void);
