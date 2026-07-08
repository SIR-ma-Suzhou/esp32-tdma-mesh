#include "sensor_pir.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"

static const char *TAG = "PIR";

static gpio_num_t g_pir_gpio = GPIO_NUM_NC;
static uint32_t g_last_trigger_ms = 0;

esp_err_t pir_sensor_init(gpio_num_t gpio_num, bool wakeup_en)
{
    g_pir_gpio = gpio_num;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // HC-SR501主动输出，无需下拉
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (wakeup_en) {
        ret = esp_sleep_enable_ext0_wakeup(gpio_num, 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "EXT0 wakeup config failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "PIR wakeup enabled on GPIO %d", gpio_num);
    }

    ESP_LOGI(TAG, "HC-SR501 PIR sensor initialized on GPIO %d", gpio_num);
    return ESP_OK;
}

bool pir_sensor_is_triggered(void)
{
    if (g_pir_gpio == GPIO_NUM_NC) return false;

    int level = gpio_get_level(g_pir_gpio);
    if (level == 1) {
        g_last_trigger_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    return (level == 1);
}

uint32_t pir_sensor_last_trigger_ms(void)
{
    return g_last_trigger_ms;
}
