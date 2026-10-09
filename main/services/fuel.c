#include "fuel.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net/http_util.h"

static const char *TAG = "fuel";

#define API_URL       "https://energia.serviciosmin.gob.es/ServiciosRestCarburantes/PreciosCarburantes/" \
                      "EstacionesTerrestres/FiltroProvinciaProducto/%d/%d"
#define MAX_PROVINCES 3
#define BORDER_KM     45   // also query provinces whose centre is this close to the nearest one's
#define MAX_BODY      (512 * 1024)

const fuel_product_t FUEL_PRODUCTS[] = {
    { 4,  "Diésel",       "Gasóleo A" },
    { 1,  "Gasolina 95",  "Gasolina 95 E5" },
    { 3,  "Gasolina 98",  "Gasolina 98 E5" },
    { 5,  "Diésel+",      "Gasóleo Premium" },
    { 17, "GLP",          "Gases licuados del petróleo" },
};
const int FUEL_PRODUCT_COUNT = sizeof(FUEL_PRODUCTS) / sizeof(FUEL_PRODUCTS[0]);

// Mean position of each province's stations (generated from the full dataset, 2026-10).
// Used to pick which provinces to query for a location.
static const struct {
    uint8_t id;
    float lat, lon;
} PROVINCES[] = {
    {  1,   42.852,   -2.695 },  // Araba/Álava (75)
    {  2,   38.945,   -1.867 },  // Albacete (157)
    {  3,   38.383,   -0.528 },  // Alicante (481)
    {  4,   37.001,   -2.401 },  // Almería (230)
    {  5,   40.633,   -4.804 },  // Ávila (72)
    {  6,   38.740,   -6.338 },  // Badajoz (283)
    {  7,   39.562,    2.838 },  // Balears (Illes) (224)
    {  8,   41.480,    2.087 },  // Barcelona (803)
    {  9,   42.324,   -3.597 },  // Burgos (137)
    { 10,   39.743,   -6.091 },  // Cáceres (160)
    { 11,   36.499,   -5.952 },  // Cádiz (293)
    { 12,   40.053,   -0.056 },  // Castellón / Castelló (199)
    { 13,   38.982,   -3.553 },  // Ciudad Real (220)
    { 14,   37.767,   -4.728 },  // Córdoba (208)
    { 15,   43.158,   -8.470 },  // Coruña (A) (290)
    { 16,   39.736,   -2.337 },  // Cuenca (120)
    { 17,   42.066,    2.838 },  // Girona (275)
    { 18,   37.189,   -3.510 },  // Granada (285)
    { 19,   40.702,   -3.008 },  // Guadalajara (94)
    { 20,   43.225,   -2.092 },  // Gipuzkoa (147)
    { 21,   37.396,   -6.879 },  // Huelva (141)
    { 22,   42.043,   -0.034 },  // Huesca (124)
    { 23,   37.967,   -3.605 },  // Jaén (214)
    { 24,   42.549,   -5.879 },  // León (168)
    { 25,   41.805,    0.893 },  // Lleida (181)
    { 26,   42.379,   -2.365 },  // Rioja (La) (83)
    { 27,   43.083,   -7.502 },  // Lugo (130)
    { 28,   40.371,   -3.692 },  // Madrid (897)
    { 29,   36.706,   -4.620 },  // Málaga (309)
    { 30,   37.893,   -1.233 },  // Murcia (465)
    { 31,   42.618,   -1.717 },  // Navarra (254)
    { 32,   42.237,   -7.733 },  // Ourense (87)
    { 33,   43.429,   -5.877 },  // Asturias (243)
    { 34,   42.234,   -4.484 },  // Palencia (72)
    { 35,   28.226,  -14.975 },  // Palmas (Las) (257)
    { 36,   42.137,   -8.395 },  // Pontevedra (229)
    { 37,   40.860,   -5.867 },  // Salamanca (112)
    { 38,   28.339,  -16.652 },  // Santa Cruz De Tenerife (242)
    { 39,   43.106,   -3.846 },  // Cantabria (167)
    { 40,   41.087,   -4.122 },  // Segovia (77)
    { 41,   37.357,   -5.794 },  // Sevilla (443)
    { 42,   41.630,   -2.526 },  // Soria (43)
    { 43,   41.061,    1.024 },  // Tarragona (230)
    { 44,   40.681,   -0.755 },  // Teruel (71)
    { 45,   39.898,   -4.038 },  // Toledo (243)
    { 46,   39.357,   -0.478 },  // Valencia / València (647)
    { 47,   41.587,   -4.788 },  // Valladolid (152)
    { 48,   43.267,   -2.880 },  // Bizkaia (132)
    { 49,   41.721,   -5.779 },  // Zamora (88)
    { 50,   41.631,   -1.029 },  // Zaragoza (240)
    { 51,   35.891,   -5.322 },  // Ceuta (10)
    { 52,   35.284,   -2.942 },  // Melilla (12)
};

const fuel_product_t *fuel_product(int id)
{
    for (int i = 0; i < FUEL_PRODUCT_COUNT; i++) {
        if (FUEL_PRODUCTS[i].id == id) {
            return &FUEL_PRODUCTS[i];
        }
    }
    return &FUEL_PRODUCTS[0];
}

float fuel_distance_km(float lat1, float lon1, float lat2, float lon2)
{
    const float r = 6371.0f, k = (float)M_PI / 180.0f;
    float dlat = (lat2 - lat1) * k, dlon = (lon2 - lon1) * k;
    float a = sinf(dlat / 2) * sinf(dlat / 2) + cosf(lat1 * k) * cosf(lat2 * k) * sinf(dlon / 2) * sinf(dlon / 2);
    return 2 * r * asinf(sqrtf(a));
}

// "1,459" -> 1.459 (the API uses Spanish decimal commas); NAN if empty.
static float parse_num(const cJSON *obj, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
    if (!s || !*s) {
        return NAN;
    }
    char buf[24];
    strlcpy(buf, s, sizeof(buf));
    for (char *p = buf; *p; p++) {
        if (*p == ',') {
            *p = '.';
        }
    }
    return strtof(buf, NULL);
}

static const char *str(const cJSON *obj, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
    return s ? s : "";
}

// "AVENIDA DE MADRID, 26" -> "Avenida De Madrid, 26". Also lowercases 2-byte UTF-8
// Latin-1 capitals (Á É Í Ó Ú Ñ Ü ...: C3 80..9E -> C3 A0..BE, except × at C3 97).
static void title_case(char *dst, const char *src, size_t len)
{
    strlcpy(dst, src, len);
    bool word_start = true;
    for (unsigned char *p = (unsigned char *)dst; *p; p++) {
        if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            if (!word_start) {
                p[1] += 0x20;
            }
            p++;
            word_start = false;
        } else if (*p >= 0x80) {
            word_start = false;  // other multibyte sequences: leave as is
        } else if (isalpha(*p)) {
            *p = word_start ? toupper(*p) : tolower(*p);
            word_start = false;
        } else {
            word_start = (*p != '\'');
        }
    }
}

// Nearest province by centre, plus any others whose centre is within BORDER_KM of that
// distance (so a location near a border also gets the neighbouring province's stations).
static int pick_provinces(float lat, float lon, int out[MAX_PROVINCES])
{
    const int n = sizeof(PROVINCES) / sizeof(PROVINCES[0]);
    float dist[sizeof(PROVINCES) / sizeof(PROVINCES[0])];
    bool used[sizeof(PROVINCES) / sizeof(PROVINCES[0])] = { 0 };
    for (int i = 0; i < n; i++) {
        dist[i] = fuel_distance_km(lat, lon, PROVINCES[i].lat, PROVINCES[i].lon);
    }
    int count = 0;
    float nearest = 0;
    while (count < MAX_PROVINCES) {
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (!used[i] && (pick < 0 || dist[i] < dist[pick])) {
                pick = i;
            }
        }
        if (count == 0) {
            nearest = dist[pick];
        } else if (dist[pick] > nearest + BORDER_KM) {
            break;
        }
        used[pick] = true;
        out[count++] = PROVINCES[pick].id;
    }
    return count;
}

static int cmp_dist(const void *a, const void *b)
{
    float d = ((const fuel_station_t *)a)->dist_km - ((const fuel_station_t *)b)->dist_km;
    return d < 0 ? -1 : d > 0;
}

static void add_stations(const cJSON *list, fuel_result_t *r, int *cap)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (strcmp(str(e, "Tipo Venta"), "P") != 0) {
            continue;  // "R" = restricted to members/fleets
        }
        float price = parse_num(e, "PrecioProducto");
        float lat = parse_num(e, "Latitud");
        float lon = parse_num(e, "Longitud (WGS84)");
        if (isnan(price) || isnan(lat) || isnan(lon) || price <= 0) {
            continue;
        }
        float d = fuel_distance_km(r->lat, r->lon, lat, lon);
        if (d > FUEL_MAX_KM) {
            continue;
        }
        if (r->count == *cap) {
            int ncap = *cap ? *cap * 2 : 128;
            fuel_station_t *n = heap_caps_realloc(r->stations, ncap * sizeof(*n), MALLOC_CAP_SPIRAM);
            if (!n) {
                return;
            }
            r->stations = n;
            *cap = ncap;
        }
        fuel_station_t *s = &r->stations[r->count++];
        memset(s, 0, sizeof(*s));
        strlcpy(s->brand, str(e, "Rótulo"), sizeof(s->brand));
        title_case(s->address, str(e, "Dirección"), sizeof(s->address));
        title_case(s->town, str(e, "Localidad"), sizeof(s->town));
        strlcpy(s->hours, str(e, "Horario"), sizeof(s->hours));
        s->lat = lat;
        s->lon = lon;
        s->price = price;
        s->dist_km = d;
        s->h24 = strstr(s->hours, "24H") != NULL;
    }
}

esp_err_t fuel_fetch(int product, float lat, float lon, fuel_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->product = product;
    out->lat = lat;
    out->lon = lon;

    int provs[MAX_PROVINCES];
    int np = pick_provinces(lat, lon, provs);
    http_session_t http = { 0 };
    int cap = 0, ok = 0;
    for (int i = 0; i < np; i++) {
        char url[200];
        snprintf(url, sizeof(url), API_URL, provs[i], product);
        char *body = NULL;
        if (http_session_get(&http, url, MAX_BODY, &body, NULL, NULL) != ESP_OK) {
            continue;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            continue;
        }
        if (!out->updated[0]) {
            strlcpy(out->updated, str(root, "Fecha"), sizeof(out->updated));
        }
        add_stations(cJSON_GetObjectItemCaseSensitive(root, "ListaEESSPrecio"), out, &cap);
        cJSON_Delete(root);
        ok++;
    }
    http_session_close(&http);
    if (ok == 0) {
        fuel_result_free(out);
        return ESP_FAIL;
    }
    qsort(out->stations, out->count, sizeof(*out->stations), cmp_dist);
    out->fetched_at = time(NULL);
    ESP_LOGI(TAG, "product %d: %d stations within %d km (%d provinces)", product, out->count, FUEL_MAX_KM, np);
    return ESP_OK;
}

void fuel_result_free(fuel_result_t *r)
{
    heap_caps_free(r->stations);
    r->stations = NULL;
    r->count = 0;
}
