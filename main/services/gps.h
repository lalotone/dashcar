#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// GT-U7 (u-blox 7 class) GPS, NMEA at 9600 baud. The board has no free TTL UART, so the GPS TX
// line goes into the CAN transceiver: CAN_H tied to VOUT (3.3 V), GPS TX on CAN_L. The TJA1051
// reads 0 V differential as recessive (= 1) and ~3.3 V as dominant (= 0), so its RXD (GPIO 16)
// reproduces the serial signal. CAN TX (GPIO 15) is held recessive; never start TWAI with this
// wiring. See ESP32-info.md §3.

typedef struct {
    bool     fix;          // a position has been received at least once since boot
    double   lat, lon;
    float    speed_kmh;    // 0 below GPS_MIN_KMH
    float    course;       // degrees true, valid when moving
    float    alt_m;        // above mean sea level (GGA), valid if has_alt
    bool     has_alt;
    int      sats;         // satellites used (GGA), -1 if unknown
    float    hdop;         // -1 if unknown
    time_t   utc;          // GPS time of the fix (0 if unknown)
    uint32_t age_s;        // seconds since that position
} gps_fix_t;

typedef struct {
    uint32_t bytes;        // received since boot
    uint32_t sentences;    // with a valid checksum
    uint32_t bad;          // checksum errors (signal problems show up here)
    int      sats_in_view; // GSV, -1 if unknown
} gps_stats_t;

typedef enum {
    GPS_NO_DATA,     // nothing (valid) received: not wired, or wiring problem
    GPS_SEARCHING,   // talking, no current fix
    GPS_FIX,         // fix within the last 5 s
} gps_state_t;

#define GPS_FRESH_S 5     // a fix older than this is "lost" (tunnel, garage)
#define GPS_MIN_KMH 5.0f  // reported speed below this is noise: shown and used as 0
#define GPS_MAX_HDOP 5.0f // a worse fix reports speed 0 (its positions wander)
#define GPS_MAX_SATS 24   // satellites kept per GSV cycle (6 sentences of 4)
#define GPS_USABLE_SNR 25 // dB-Hz: below about this a receiver can't download a satellite's orbit data

typedef struct {
    uint8_t prn;
    uint8_t snr;           // dB-Hz, 0 = listed by the receiver but not received
    bool    used;          // part of the current fix (GSA)
} gps_sat_t;

typedef struct {
    int      listed;       // satellites in the table
    int      received;     // with any signal
    int      usable;       // with snr >= GPS_USABLE_SNR
    int      used;         // part of the current fix
    int      best_snr;     // strongest signal, 0 if none
    uint32_t no_fix_s;     // seconds since the last fix (or the first sentence, if none yet)
} gps_signal_t;

void gps_init(void);
// Test only (debug/soak.c): replaces the receiver with simulated fixes, ~1/s, until reboot.
void gps_sim_feed(double lat, double lon, float kmh, float course);
// Current state; *sats = satellites used (fix) or in view (searching), -1 if unknown.
gps_state_t gps_state(int *sats);
bool gps_get(gps_fix_t *out);          // false if never had a fix
void gps_get_stats(gps_stats_t *out);
// Latest complete GSV cycle, by PRN. `out` may be NULL or holds GPS_MAX_SATS; returns the count.
int gps_get_sats(gps_sat_t *out, gps_signal_t *signal);
// "No data from the GPS", "Searching (5 satellites in view)", "Fix, 9 satellites"...
void gps_status_text(char *buf, size_t len);
