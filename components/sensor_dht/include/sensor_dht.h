#ifndef DHT_H
#define DHT_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHT_TYPE_DHT11 = 0,
    DHT_TYPE_DHT22,
    DHT_TYPE_AM2301
} dht_sensor_type_t;

esp_err_t dht_read_float_data(dht_sensor_type_t sensor_type, gpio_num_t gpio_num,
                               float *humidity, float *temperature);

#ifdef __cplusplus
}
#endif

#endif
