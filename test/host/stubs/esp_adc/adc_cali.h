// Host stub: the ESP-IDF ADC calibration API used by clu_localio_sensors.c (the test implements it).
#pragma once
#include "esp_err.h"
typedef struct adc_cali_scheme_t *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *voltage);
