#pragma once

#include <stdint.h>
#include <time.h>

// Days since 1970-01-01 for a proleptic Gregorian date (newlib has no timegm()).
int64_t days_from_civil(int y, int m, int d);

// ISO 8601 -> UTC epoch: "2026-10-08T10:00:00+00:00", "...T08:05:19.000+01:00", "...Z".
// Returns 0 if unparsable.
time_t iso8601_to_utc(const char *s);
