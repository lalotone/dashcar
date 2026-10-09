#include "parking.h"

#include <stdio.h>
#include "settings.h"

// TODO(GPS): placeholder until the GPS module saves real spots (Plaza del Pilar, Zaragoza).
#define DEMO_LAT 41.65660
#define DEMO_LON (-0.87890)

void parking_get(parking_spot_t *out)
{
    if (settings_get_parking(&out->lat, &out->lon, &out->saved_at)) {
        out->demo = false;
        return;
    }
    out->lat = DEMO_LAT;
    out->lon = DEMO_LON;
    out->saved_at = 0;
    out->demo = true;
}

void parking_save(double lat, double lon)
{
    settings_set_parking(lat, lon, time(NULL));
}

void parking_walk_url(const parking_spot_t *p, char *buf, size_t len)
{
    snprintf(buf, len, "https://www.google.com/maps/dir/?api=1&destination=%.6f,%.6f&travelmode=walking",
             p->lat, p->lon);
}
