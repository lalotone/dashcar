#include "gps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "services/ext_rtc.h"
#include "services/net_worker.h"
#include "services/parking.h"
#include "services/timeutil.h"
#include "services/trip.h"

static const char *TAG = "gps";

#define GPS_UART        UART_NUM_1
#define GPS_RX_GPIO     16      // CAN transceiver RXD (see gps.h)
#define CAN_TX_GPIO     15      // held high = recessive, so the transceiver never drives the bus
#define GPS_BAUD        9600
#define POLL_MS         200     // ~200 bytes arrive per poll at 9600 baud; the driver buffers 1 KB
#define STATS_LOG_S     60

// Parking: a stop of PARK_STOP_S after driving saves the spot. The screen goes off with the
// ignition, so the last such stop is where the car was left.
#define MOVING_KMH      10.0f
#define STOPPED_KMH     3.0f
#define PARK_STOP_S     90

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gps_fix_t s_fix = { .sats = -1, .hdop = -1 };
static int64_t s_fix_us;        // esp_timer time of s_fix
static int s_gga_sats = -1;     // from GGA, merged into the next RMC fix
static float s_gga_hdop = -1;
static float s_gga_alt;
static bool s_gga_has_alt;
static gps_stats_t s_stats = { .sats_in_view = -1 };
static int64_t s_last_sentence_us;

static char s_line[96];         // NMEA sentences are at most 82 characters
static size_t s_line_len;
static bool s_line_overflow;

static bool s_moved;            // has driven since the last saved stop
static int64_t s_stopped_since; // 0 = moving or unknown
static bool s_clock_set;
static bool s_sim;              // gps_sim_feed() took over (soak test): ignore the receiver

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Split a sentence body in place at commas. Empty fields stay as "".
static int split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) {
        if (*s == ',') {
            *s = '\0';
            f[n++] = s + 1;
        }
    }
    return n;
}

// "4139.3960" + "N" -> 41.656600
static bool parse_coord(const char *v, const char *hemi, double *out)
{
    if (!*v || !*hemi) {
        return false;
    }
    const double raw = atof(v);
    const int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    if (*hemi == 'S' || *hemi == 'W') {
        d = -d;
    }
    *out = d;
    return true;
}

static void job_set_clock(void *arg)
{
    const struct timeval tv = { .tv_sec = (time_t)(intptr_t)arg };
    settimeofday(&tv, NULL);
    ext_rtc_save_now();
    ESP_LOGI(TAG, "system time set from GPS");
}

static double s_park_lat, s_park_lon;

static void job_save_parking(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    const double lat = s_park_lat, lon = s_park_lon;
    portEXIT_CRITICAL(&s_mux);
    parking_save(lat, lon);
    ESP_LOGI(TAG, "parked at %.6f,%.6f", lat, lon);
}

static void update_parking(double lat, double lon, float kmh, int64_t now)
{
    if (kmh >= MOVING_KMH) {
        s_moved = true;
        s_stopped_since = 0;
    } else if (kmh < STOPPED_KMH) {
        if (!s_stopped_since) {
            s_stopped_since = now;
        } else if (s_moved && now - s_stopped_since >= PARK_STOP_S * 1000000LL) {
            s_moved = false;
            portENTER_CRITICAL(&s_mux);
            s_park_lat = lat;
            s_park_lon = lon;
            portEXIT_CRITICAL(&s_mux);
            net_worker_post(job_save_parking, NULL);  // NVS write: not from the timer task
        }
    }
}

// $GPRMC,hhmmss.ss,A,llll.ll,N,yyyyy.yy,E,knots,course,ddmmyy,,,A*cs
static void on_rmc(char **f, int n)
{
    if (s_sim || n < 10 || f[2][0] != 'A') {
        return;  // 'V' = no fix yet
    }
    double lat, lon;
    if (!parse_coord(f[3], f[4], &lat) || !parse_coord(f[5], f[6], &lon)) {
        return;
    }
    // Standing still, the receiver reports 1-4 km/h of noise (worse with few satellites): treat
    // anything below GPS_MIN_KMH, or any speed from a poor fix, as stopped, for every user.
    float kmh = atof(f[7]) * 1.852f;
    if (kmh < GPS_MIN_KMH || (s_gga_hdop > GPS_MAX_HDOP)) {
        kmh = 0;
    }
    const int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    const bool first = !s_fix.fix;
    s_fix.fix = true;
    s_fix.lat = lat;
    s_fix.lon = lon;
    s_fix.speed_kmh = kmh;
    s_fix.course = f[8][0] ? atof(f[8]) : 0;
    s_fix.sats = s_gga_sats;
    s_fix.hdop = s_gga_hdop;
    s_fix.alt_m = s_gga_alt;
    s_fix.has_alt = s_gga_has_alt;
    s_fix.utc = 0;
    s_fix_us = now;
    portEXIT_CRITICAL(&s_mux);
    if (first) {
        ESP_LOGI(TAG, "first fix: %.6f,%.6f", lat, lon);
    }

    const char *t = f[1], *d = f[9];
    if (strlen(t) >= 6 && strlen(d) == 6) {
        const int day = (d[0] - '0') * 10 + d[1] - '0', mon = (d[2] - '0') * 10 + d[3] - '0';
        const int year = 2000 + (d[4] - '0') * 10 + d[5] - '0';
        const int hh = (t[0] - '0') * 10 + t[1] - '0', mm = (t[2] - '0') * 10 + t[3] - '0';
        const int ss = (t[4] - '0') * 10 + t[5] - '0';
        if (year >= 2025 && year <= 2040) {
            const time_t epoch = days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
            portENTER_CRITICAL(&s_mux);
            s_fix.utc = epoch;
            portEXIT_CRITICAL(&s_mux);
            // No network (no hotspot in the car): the GPS still knows the time.
            if (!s_clock_set && time(NULL) < 1700000000) {
                s_clock_set = net_worker_post(job_set_clock, (void *)(intptr_t)epoch);
            }
        }
    }

    update_parking(lat, lon, kmh, now);
    trip_on_fix(lat, lon, kmh);
}

// $GPGGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,...
static void on_gga(char **f, int n)
{
    if (n < 9) {
        return;
    }
    const bool fix = f[6][0] && f[6][0] != '0';
    s_gga_sats = fix && f[7][0] ? atoi(f[7]) : 0;
    s_gga_hdop = f[8][0] ? atof(f[8]) : -1;
    s_gga_has_alt = fix && n > 9 && f[9][0];
    s_gga_alt = s_gga_has_alt ? atof(f[9]) : 0;
}

// $GPGSV,total,index,in_view,...
static void on_gsv(char **f, int n)
{
    if (n >= 4 && f[3][0]) {
        s_stats.sats_in_view = atoi(f[3]);
    }
}

static void on_line(char *s)
{
    // $<talker+type>,...*HH
    char *star = strrchr(s, '*');
    if (s[0] != '$' || !star || hexval(star[1]) < 0 || hexval(star[2]) < 0) {
        s_stats.bad++;
        return;
    }
    uint8_t sum = 0;
    for (const char *p = s + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    if (sum != (hexval(star[1]) << 4 | hexval(star[2]))) {
        s_stats.bad++;
        return;
    }
    s_stats.sentences++;
    s_last_sentence_us = esp_timer_get_time();
    *star = '\0';
    if (s_stats.sentences == 1) {
        ESP_LOGI(TAG, "receiving NMEA: %s", s);
    }
    char *f[20];
    const int n = split(s + 1, f, 20);
    if (strlen(f[0]) != 5) {
        return;
    }
    const char *type = f[0] + 2;  // skip the talker (GP, GN, GL...)
    if (!strcmp(type, "RMC")) {
        on_rmc(f, n);
    } else if (!strcmp(type, "GGA")) {
        on_gga(f, n);
    } else if (!strcmp(type, "GSV")) {
        on_gsv(f, n);
    }
}

static void poll_cb(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    int n;
    while ((n = uart_read_bytes(GPS_UART, buf, sizeof(buf), 0)) > 0) {
        s_stats.bytes += n;
        for (int i = 0; i < n; i++) {
            const char c = buf[i];
            if (c == '\r' || c == '\n') {
                if (s_line_len && !s_line_overflow) {
                    s_line[s_line_len] = '\0';
                    on_line(s_line);
                } else if (s_line_overflow) {
                    s_stats.bad++;
                }
                s_line_len = 0;
                s_line_overflow = false;
            } else if (s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
            } else {
                s_line_overflow = true;
            }
        }
    }

    static int64_t last_log;
    const int64_t now = esp_timer_get_time();
    if (now - last_log >= STATS_LOG_S * 1000000LL) {
        last_log = now;
        char status[64];
        gps_status_text(status, sizeof(status));
        ESP_LOGI(TAG, "%s (%u bytes, %u sentences, %u bad)", status, (unsigned)s_stats.bytes,
                 (unsigned)s_stats.sentences, (unsigned)s_stats.bad);
    }
}

void gps_sim_feed(double lat, double lon, float kmh, float course)
{
    const int64_t now = esp_timer_get_time();
    const time_t utc = time(NULL);  // not inside the critical section: time() takes a lock (abort())
    s_sim = true;
    if (kmh < GPS_MIN_KMH) {
        kmh = 0;
    }
    portENTER_CRITICAL(&s_mux);
    s_fix = (gps_fix_t){
        .fix = true, .lat = lat, .lon = lon, .speed_kmh = kmh, .course = course,
        .alt_m = 400, .has_alt = true, .sats = 9, .hdop = 1.0f, .utc = utc,
    };
    s_fix_us = now;
    portEXIT_CRITICAL(&s_mux);
    s_last_sentence_us = now;
    s_stats.sentences++;
    update_parking(lat, lon, kmh, now);
    trip_on_fix(lat, lon, kmh);
}

void gps_init(void)
{
    // Keep the CAN transceiver in receive-only (recessive) state: CAN_H is wired to 3.3 V.
    gpio_set_level(CAN_TX_GPIO, 1);
    gpio_set_direction(CAN_TX_GPIO, GPIO_MODE_OUTPUT);

    const uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(GPS_UART, 1024, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(GPS_UART, &cfg) != ESP_OK ||
        uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, GPS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART setup failed");
        return;
    }
    const esp_timer_create_args_t targs = { .callback = poll_cb, .name = "gps" };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) != ESP_OK || esp_timer_start_periodic(t, POLL_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "timer setup failed");
    }
}

bool gps_get(gps_fix_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_fix;
    const int64_t at = s_fix_us;
    portEXIT_CRITICAL(&s_mux);
    out->age_s = out->fix ? (uint32_t)((esp_timer_get_time() - at) / 1000000) : 0;
    return out->fix;
}

void gps_get_stats(gps_stats_t *out)
{
    *out = s_stats;
}

gps_state_t gps_state(int *sats)
{
    gps_fix_t f;
    gps_get(&f);
    // No sentence in the last few seconds means the GPS went quiet (unplugged).
    const bool talking = s_stats.sentences && esp_timer_get_time() - s_last_sentence_us < 3000000;
    if (!talking) {
        *sats = -1;
        return GPS_NO_DATA;
    }
    if (f.fix && f.age_s < GPS_FRESH_S) {
        *sats = f.sats > 0 ? f.sats : -1;  // GGA can lag RMC and report 0 used
        return GPS_FIX;
    }
    *sats = s_stats.sats_in_view;
    return GPS_SEARCHING;
}

void gps_status_text(char *buf, size_t len)
{
    gps_fix_t f;
    gps_get(&f);
    int sats;
    const gps_state_t st = gps_state(&sats);
    if (st == GPS_NO_DATA) {
        snprintf(buf, len, s_stats.bytes && !s_stats.sentences ? "GPS signal garbled (check wiring)" :
                           "No data from the GPS");
    } else if (st == GPS_FIX) {
        if (f.sats > 0) {
            snprintf(buf, len, "Fix, %d satellites", f.sats);
        } else {
            snprintf(buf, len, "Fix");
        }
    } else if (s_stats.sats_in_view >= 0) {
        snprintf(buf, len, "Searching (%d satellites in view)", s_stats.sats_in_view);
    } else {
        snprintf(buf, len, "Searching");
    }
}
