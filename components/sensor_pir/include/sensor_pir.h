#ifndef SENSOR_PIR_H
#define SENSOR_PIR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pir_sensor_init(gpio_num_t gpio_num, bool wakeup_en);

bool pir_sensor_is_triggered(void);

uint32_t pir_sensor_last_trigger_ms(void);

#ifdef __cplusplus
}
#endif

#endif
