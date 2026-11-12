/**
 * @file pcf85063.c
 * @brief PCF85063 time register encoding (BCD <-> UTC epoch), independent of TZ and libc timegm.
 */
#include "pcf85063.h"

static int bcd(uint8_t v, uint8_t mask, int *out)
{
    v &= mask;
    if ((v & 0x0f) > 9 || (v >> 4) > 9) {
        return 0;
    }
    *out = (v >> 4) * 10 + (v & 0x0f);
    return 1;
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/** @brief Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/** @brief Inverse of days_from_civil(). */
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static int days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return dm[m - 1] + (m == 2 && leap);
}

bool pcf85063_decode(const uint8_t r[PCF85063_TIME_LEN], time_t *out)
{
    int sec, min, hour, day, mon, year;
    if (r[0] & PCF85063_SECONDS_OS) {
        return false;
    }
    if (!bcd(r[0], 0x7f, &sec) || !bcd(r[1], 0x7f, &min) || !bcd(r[2], 0x3f, &hour) ||
        !bcd(r[3], 0x3f, &day) || !bcd(r[5], 0x1f, &mon) || !bcd(r[6], 0xff, &year)) {
        return false;
    }
    year += 2000;
    if (sec > 59 || min > 59 || hour > 23 || mon < 1 || mon > 12 || day < 1 ||
        day > days_in_month(year, mon)) {
        return false;
    }
    *out = (time_t)(days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec);
    return true;
}

bool pcf85063_encode(time_t t, uint8_t r[PCF85063_TIME_LEN])
{
    int64_t days = (int64_t)t / 86400, secs = (int64_t)t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    if (y < 2000 || y > 2099) {
        return false;
    }
    r[0] = to_bcd((int)(secs % 60));
    r[1] = to_bcd((int)(secs / 60 % 60));
    r[2] = to_bcd((int)(secs / 3600));
    r[3] = to_bcd(d);
    r[4] = (uint8_t)(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday (4); 0 = Sunday
    r[5] = to_bcd(m);
    r[6] = to_bcd(y - 2000);
    return true;
}
