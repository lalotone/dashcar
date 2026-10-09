#include "ext_rtc.h"

#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "services/timeutil.h"

static const char *TAG = "rtc";

#define PCF85063_ADDR     0x51
#define REG_SECONDS       0x04   // seconds..years: 0x04..0x0A, BCD
#define SECONDS_OS_FLAG   0x80   // oscillator stopped: time is not valid

static i2c_master_dev_handle_t s_dev;
static ext_rtc_state_t s_state = EXT_RTC_ABSENT;

static uint8_t bcd2bin(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint8_t bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

ext_rtc_state_t ext_rtc_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return s_state = EXT_RTC_ABSENT;
    }
    uint8_t reg = REG_SECONDS, r[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, r, sizeof(r), 100) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A not responding");
        return s_state = EXT_RTC_ABSENT;
    }
    const int year = 2000 + bcd2bin(r[6]);
    // Out-of-range years are leftovers (the factory demo set 2054): a wrong clock would break
    // TLS certificate checks, so treat them as unset until SNTP corrects the RTC.
    if ((r[0] & SECONDS_OS_FLAG) || year < 2025 || year > 2040) {
        ESP_LOGW(TAG, "RTC time not valid (oscillator stopped or never set)");
        return s_state = EXT_RTC_INVALID;
    }
    const int64_t days = days_from_civil(year, bcd2bin(r[5] & 0x1F), bcd2bin(r[3] & 0x3F));
    const struct timeval tv = {
        .tv_sec = days * 86400 + bcd2bin(r[2] & 0x3F) * 3600 + bcd2bin(r[1] & 0x7F) * 60 + bcd2bin(r[0] & 0x7F),
    };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC", year, bcd2bin(r[5] & 0x1F),
             bcd2bin(r[3] & 0x3F), bcd2bin(r[2] & 0x3F), bcd2bin(r[1] & 0x7F), bcd2bin(r[0] & 0x7F));
    return s_state = EXT_RTC_VALID;
}

void ext_rtc_save_now(void)
{
    if (!s_dev) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    const uint8_t buf[8] = {
        REG_SECONDS,
        bin2bcd(tm.tm_sec),   // also clears the OS flag
        bin2bcd(tm.tm_min),
        bin2bcd(tm.tm_hour),  // 24 h mode (Control_1 12_24 bit = 0, the default)
        bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        bin2bcd(tm.tm_mon + 1),
        bin2bcd(tm.tm_year % 100),
    };
    if (i2c_master_transmit(s_dev, buf, sizeof(buf), 100) == ESP_OK) {
        if (s_state != EXT_RTC_VALID) {
            ESP_LOGI(TAG, "RTC set from network time");
        }
        s_state = EXT_RTC_VALID;
    } else {
        ESP_LOGW(TAG, "RTC write failed");
    }
}

ext_rtc_state_t ext_rtc_state(void)
{
    return s_state;
}
