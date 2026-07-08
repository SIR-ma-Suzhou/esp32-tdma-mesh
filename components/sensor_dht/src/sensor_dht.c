#include "sensor_dht.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "DHT";

#define DHT_TIMEOUT_US 100

static esp_err_t dht_read_data(dht_sensor_type_t sensor_type, gpio_num_t gpio_num,
                               uint8_t *data) {
    // 1. MCU发送开始信号：拉低至少 1ms
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // DHT22 要求>1ms，用FreeRTOS延时让出CPU给WiFi/BLE
    gpio_set_level(gpio_num, 1);
    ets_delay_us(40);
    gpio_set_direction(gpio_num, GPIO_MODE_INPUT);

    // 2. 等待DHT响应
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 0) {
        if (esp_timer_get_time() - start > DHT_TIMEOUT_US) return ESP_ERR_TIMEOUT;
    }
    start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == 1) {
        if (esp_timer_get_time() - start > DHT_TIMEOUT_US) return ESP_ERR_TIMEOUT;
    }

    // 3. 读取40位数据
    for (int i = 0; i < 40; i++) {
        start = esp_timer_get_time();
        while (gpio_get_level(gpio_num) == 0) {
            if (esp_timer_get_time() - start > DHT_TIMEOUT_US) return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(30); // 等待电平稳定，再判断高低
        if (gpio_get_level(gpio_num) == 1) {
            data[i / 8] <<= 1;
            data[i / 8] |= 1;
        } else {
            data[i / 8] <<= 1;
        }
        start = esp_timer_get_time();
        while (gpio_get_level(gpio_num) == 1) {
            if (esp_timer_get_time() - start > DHT_TIMEOUT_US) break;
        }
    }

    // 4. 打印原始数据，方便你检查
    ESP_LOGI(TAG, "Raw: %02x %02x %02x %02x %02x", data[0], data[1], data[2], data[3], data[4]);

    // 5. 校验
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGE(TAG, "CRC: calc=%02x recv=%02x", checksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

esp_err_t dht_read_float_data(dht_sensor_type_t sensor_type, gpio_num_t gpio_num,
                               float *humidity, float *temperature) {
    uint8_t data[5] = {0};
    esp_err_t ret;

    // DHT22 单总线时序容易被WiFi/BLE中断，最多重试3次
    for (int retry = 0; retry < 3; retry++) {
        memset(data, 0, sizeof(data));
        ret = dht_read_data(sensor_type, gpio_num, data);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));  // 重试前稍等
    }

    if (ret != ESP_OK) {
        return ret;
    }

    if (sensor_type == DHT_TYPE_DHT11) {
        *humidity = (float)data[0];
        *temperature = (float)data[2];
    } else {
        *humidity = ((float)((data[0] << 8) | data[1])) / 10.0f;
        *temperature = ((float)(((data[2] & 0x7F) << 8) | data[3])) / 10.0f;
    }

    return ESP_OK;
}
