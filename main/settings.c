#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "dashcar";

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reset (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX])
{
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = get_str(h, "ssid", ssid, SETTINGS_SSID_MAX) && ssid[0] != '\0';
    get_str(h, "pass", pass, SETTINGS_PASS_MAX);
    nvs_close(h);
    return ok;
}

void settings_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

void settings_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

void settings_get_location(settings_location_t *loc)
{
    memset(loc, 0, sizeof(*loc));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(loc->lat);
    bool ok = nvs_get_blob(h, "loc_lat", &loc->lat, &n) == ESP_OK;
    n = sizeof(loc->lon);
    ok = ok && nvs_get_blob(h, "loc_lon", &loc->lon, &n) == ESP_OK;
    ok = ok && get_str(h, "loc_name", loc->name, sizeof(loc->name));
    loc->valid = ok;
    nvs_close(h);
}

void settings_set_location(const settings_location_t *loc)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (loc && loc->valid) {
        nvs_set_blob(h, "loc_lat", &loc->lat, sizeof(loc->lat));
        nvs_set_blob(h, "loc_lon", &loc->lon, sizeof(loc->lon));
        nvs_set_str(h, "loc_name", loc->name);
    } else {
        nvs_erase_key(h, "loc_lat");
        nvs_erase_key(h, "loc_lon");
        nvs_erase_key(h, "loc_name");
    }
    nvs_commit(h);
    nvs_close(h);
}

bool settings_get_imperial(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "imperial", &v);
        nvs_close(h);
    }
    return v != 0;
}

void settings_set_imperial(bool imperial)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "imperial", imperial ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t get_u8(const char *key, uint8_t def)
{
    uint8_t v = def;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void set_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

int settings_get_fuel_product(void)
{
    return get_u8("fuel_prod", 4);
}

void settings_set_fuel_product(int id)
{
    set_u8("fuel_prod", (uint8_t)id);
}

int settings_get_fuel_radius(void)
{
    return get_u8("fuel_km", 10);
}

void settings_set_fuel_radius(int km)
{
    set_u8("fuel_km", (uint8_t)km);
}

night_mode_t settings_get_night_mode(void)
{
    uint8_t v = get_u8("night", NIGHT_MODE_AUTO);
    return v <= NIGHT_MODE_OFF ? (night_mode_t)v : NIGHT_MODE_AUTO;
}

void settings_set_night_mode(night_mode_t mode)
{
    set_u8("night", (uint8_t)mode);
}

bool settings_get_ota_enabled(void)
{
    return get_u8("ota", 1) != 0;
}

void settings_set_ota_enabled(bool on)
{
    set_u8("ota", on ? 1 : 0);
}

int32_t settings_get_utc_offset(void)
{
    int32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "utc_off", &v);
        nvs_close(h);
    }
    return v;
}

void settings_set_utc_offset(int32_t offset)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, "utc_off", offset);
    nvs_commit(h);
    nvs_close(h);
}

bool settings_get_parking(double *lat, double *lon, time_t *saved_at)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof(*lat);
    bool ok = nvs_get_blob(h, "park_lat", lat, &n) == ESP_OK;
    n = sizeof(*lon);
    ok = ok && nvs_get_blob(h, "park_lon", lon, &n) == ESP_OK;
    int64_t t = 0;
    if (ok && nvs_get_i64(h, "park_t", &t) == ESP_OK) {
        *saved_at = (time_t)t;
    } else {
        *saved_at = 0;
    }
    nvs_close(h);
    return ok;
}

void settings_set_parking(double lat, double lon, time_t saved_at)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "park_lat", &lat, sizeof(lat));
    nvs_set_blob(h, "park_lon", &lon, sizeof(lon));
    nvs_set_i64(h, "park_t", (int64_t)saved_at);
    nvs_commit(h);
    nvs_close(h);
}

bool settings_get_cache(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = 0;
    bool ok = nvs_get_blob(h, key, NULL, &n) == ESP_OK && n == len && nvs_get_blob(h, key, buf, &n) == ESP_OK;
    nvs_close(h);
    return ok;
}

void settings_set_cache(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, key, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "cache %s not saved", key);
    }
    nvs_commit(h);
    nvs_close(h);
}
