#include "sensor_batt.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

static const char *TAG = "BATT";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static adc_unit_t s_adc_unit = ADC_UNIT_1;
static adc_channel_t s_adc_channel;
static float s_scale = 1.0f;
static bool s_ready = false;

esp_err_t batt_sensor_init(gpio_num_t gpio_num, float scale)
{
    s_scale = (scale > 0.1f) ? scale : 1.0f;

    // ESP32-S3 GPIO → ADC channel 映射
    // ADC1: GPIO1→CH0, GPIO2→CH1, GPIO3→CH2, GPIO4→CH3, ..., GPIO10→CH9
    // ADC2: GPIO11→CH0, GPIO12→CH1, ..., GPIO20→CH9
    if (gpio_num >= GPIO_NUM_1 && gpio_num <= GPIO_NUM_10) {
        s_adc_unit = ADC_UNIT_1;
        s_adc_channel = (adc_channel_t)(gpio_num - 1);
    } else if (gpio_num >= GPIO_NUM_11 && gpio_num <= GPIO_NUM_20) {
        s_adc_unit = ADC_UNIT_2;
        s_adc_channel = (adc_channel_t)(gpio_num - 11);
    } else {
        ESP_LOGE(TAG, "GPIO %d does not support ADC", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    // ADC1 初始化
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,    // 0~3.1V 量程（分压后, v5.1+ 推荐）
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 校准（有曲线拟合就做，没有也能用）
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_adc_unit,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration not available, using raw readings");
        s_cali_handle = NULL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Battery ADC initialized on GPIO %d, scale=%.2f", gpio_num, scale);
    return ESP_OK;
}

uint32_t batt_sensor_read_mv(void)
{
    if (!s_ready || !s_adc_handle) return 0;

    int raw = 0;
    int voltage_mv = 0;

    // 多次采样取平均
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
        sum += raw;
    }
    raw = sum / 8;

    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &voltage_mv);
    } else {
        // 无校准：12bit 衰减11dB ≈ 0~3100mV
        voltage_mv = (raw * 3100) / 4095;
    }

    return (uint32_t)((float)voltage_mv * s_scale);
}
