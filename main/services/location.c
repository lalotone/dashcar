#include "location.h"

#include <string.h>
#include "services/weather.h"
#include "settings.h"

bool location_get(location_t *out)
{
    memset(out, 0, sizeof(*out));
    // TODO(GPS): return the latest GPS fix here when it's recent enough.
    if (!weather_location(&out->lat, &out->lon, out->name, sizeof(out->name))) {
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
