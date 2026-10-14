#include "timeutil.h"

#include <stdio.h>
#include <stdlib.h>

int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

time_t iso8601_to_utc(const char *s)
{
    int y, mo, d, h, mi, se, n = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n) < 6) {
        return 0;
    }
    const char *p = s + n;
    if (*p == '.') {  // fractional seconds
        p++;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    int off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%d:%d", &oh, &om);
        off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
    }
    return (time_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off);
}
