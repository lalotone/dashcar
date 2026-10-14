#include "location.h"

#include <string.h>
#include "services/gps.h"
#include "services/weather.h"
#include "settings.h"

// A lost fix (tunnel, underground car park) keeps the last GPS position for a while: it's still
// far better than the IP location, which can be tens of km off.
#define GPS_MAX_AGE_S (30 * 60)

bool location_get(location_t *out)
{
    memset(out, 0, sizeof(*out));
    const bool have_wx = weather_location(&out->lat, &out->lon, out->name, sizeof(out->name));
    gps_fix_t fix;
    if (gps_get(&fix) && fix.age_s < GPS_MAX_AGE_S) {
        out->lat = fix.lat;
        out->lon = fix.lon;
        out->source = LOCATION_GPS;
        if (!have_wx) {
            strlcpy(out->name, "GPS position", sizeof(out->name));
        }
        return true;
    }
    if (!have_wx) {
        return false;
    }
    settings_location_t chosen;
    settings_get_location(&chosen);
    out->source = chosen.valid ? LOCATION_CITY : LOCATION_IP;
    return true;
}

const char *location_source_name(location_source_t s)
{
    switch (s) {
    case LOCATION_IP:   return "approximate (IP)";
    case LOCATION_CITY: return "chosen city";
    case LOCATION_GPS:  return "GPS";
    default:            return "unknown";
    }
}
