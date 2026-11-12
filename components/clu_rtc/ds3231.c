/**
 * @file ds3231.c
 * @brief DS3231 time register encoding: the same BCD fields as the PCF85063 in another order, so
 * it is mapped onto pcf85063_decode() / pcf85063_encode().
 *
 * PCF85063: Seconds Minutes Hours Days Weekdays(0-6) Months Years
 * DS3231  : Seconds Minutes Hours Day(1-7)  Date    Month  Year
 */
#include "ds3231.h"

#include "pcf85063.h"

bool ds3231_decode(const uint8_t r[DS3231_TIME_LEN], time_t *out)
{
    if ((r[2] & DS3231_HOURS_12H) || (r[5] & DS3231_MONTH_CENTURY)) {
        return false;
    }
    const uint8_t p[PCF85063_TIME_LEN] = {
        (uint8_t)(r[0] & 0x7f), r[1], r[2], r[4], 0, r[5], r[6],
    };
    return pcf85063_decode(p, out);
}

bool ds3231_encode(time_t t, uint8_t r[DS3231_TIME_LEN])
{
    uint8_t p[PCF85063_TIME_LEN];
    if (!pcf85063_encode(t, p)) {
        return false;
    }
    r[0] = p[0];
    r[1] = p[1];
    r[2] = p[2];          // 24-hour mode: bit 6 clear
    r[3] = (uint8_t)(p[4] + 1); // weekday 1-7, 1 = Sunday
    r[4] = p[3];
    r[5] = p[5];          // Century clear
    r[6] = p[6];
    return true;
}
