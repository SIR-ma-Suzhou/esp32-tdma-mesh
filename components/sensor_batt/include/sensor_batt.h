#ifndef SENSOR_BATT_H
#define SENSOR_BATT_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t batt_sensor_init(gpio_num_t gpio_num, float scale);

uint32_t batt_sensor_read_mv(void);

#ifdef __cplusplus
}
#endif

#endif
