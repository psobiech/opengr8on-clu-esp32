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

bool ds3231_decode(const uint8_t register_values[DS3231_TIME_LEN], time_t *out)
{
    if ((register_values[2] & DS3231_HOURS_12H) || (register_values[5] & DS3231_MONTH_CENTURY)) {
        return false;
    }
    const uint8_t pcf_registers[PCF85063_TIME_LEN] = {
        (uint8_t)(register_values[0] & 0x7f), register_values[1], register_values[2], register_values[4], 0, register_values[5], register_values[6],
    };
    return pcf85063_decode(pcf_registers, out);
}

bool ds3231_encode(time_t timestamp, uint8_t register_values[DS3231_TIME_LEN])
{
    uint8_t pcf_registers[PCF85063_TIME_LEN];
    if (!pcf85063_encode(timestamp, pcf_registers)) {
        return false;
    }
    register_values[0] = pcf_registers[0];
    register_values[1] = pcf_registers[1];
    register_values[2] = pcf_registers[2];          // 24-hour mode: bit 6 clear
    register_values[3] = (uint8_t)(pcf_registers[4] + 1); // weekday 1-7, 1 = Sunday
    register_values[4] = pcf_registers[3];
    register_values[5] = pcf_registers[5];          // Century clear
    register_values[6] = pcf_registers[6];
    return true;
}
