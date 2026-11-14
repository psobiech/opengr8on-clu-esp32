/**
 * @file pcf85063.c
 * @brief PCF85063 time register encoding (BCD <-> UTC epoch), independent of TZ and libc timegm.
 */
#include "pcf85063.h"

static int bcd(uint8_t bcd_value, uint8_t mask, int *out)
{
    bcd_value &= mask;
    if ((bcd_value & 0x0f) > 9 || (bcd_value >> 4) > 9) {
        return 0;
    }
    *out = (bcd_value >> 4) * 10 + (bcd_value & 0x0f);
    return 1;
}

static uint8_t to_bcd(int value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/** @brief Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int civil_year, int civil_month, int civil_day)
{
    civil_year -= civil_month <= 2;
    int64_t era = (civil_year >= 0 ? civil_year : civil_year - 399) / 400;
    int yoe = (int)(civil_year - era * 400);
    int doy = (153 * (civil_month + (civil_month > 2 ? -3 : 9)) + 2) / 5 + civil_day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/** @brief Inverse of days_from_civil(). */
static void civil_from_days(int64_t days_since_epoch, int *year_output, int *month_output, int *day_output)
{
    days_since_epoch += 719468;
    int64_t era = (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097;
    int doe = (int)(days_since_epoch - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int shifted_month = (5 * doy + 2) / 153;
    *day_output = doy - (153 * shifted_month + 2) / 5 + 1;
    *month_output = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    *year_output = (int)(yoe + era * 400) + (*month_output <= 2);
}

static int days_in_month(int calendar_year, int calendar_month)
{
    static const int days_in_month_table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (calendar_year % 4 == 0 && calendar_year % 100 != 0) || calendar_year % 400 == 0;
    return days_in_month_table[calendar_month - 1] + (calendar_month == 2 && leap);
}

bool pcf85063_decode(const uint8_t register_values[PCF85063_TIME_LEN], time_t *out)
{
    int sec, min, hour, day, mon, year;
    if (register_values[0] & PCF85063_SECONDS_OS) {
        return false;
    }
    if (!bcd(register_values[0], 0x7f, &sec) || !bcd(register_values[1], 0x7f, &min) || !bcd(register_values[2], 0x3f, &hour) ||
        !bcd(register_values[3], 0x3f, &day) || !bcd(register_values[5], 0x1f, &mon) || !bcd(register_values[6], 0xff, &year)) {
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

bool pcf85063_encode(time_t timestamp, uint8_t register_values[PCF85063_TIME_LEN])
{
    int64_t days = (int64_t)timestamp / 86400, secs = (int64_t)timestamp % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    int calendar_year, calendar_month, calendar_day;
    civil_from_days(days, &calendar_year, &calendar_month, &calendar_day);
    if (calendar_year < 2000 || calendar_year > 2099) {
        return false;
    }
    register_values[0] = to_bcd((int)(secs % 60));
    register_values[1] = to_bcd((int)(secs / 60 % 60));
    register_values[2] = to_bcd((int)(secs / 3600));
    register_values[3] = to_bcd(calendar_day);
    register_values[4] = (uint8_t)(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday (4); 0 = Sunday
    register_values[5] = to_bcd(calendar_month);
    register_values[6] = to_bcd(calendar_year - 2000);
    return true;
}
