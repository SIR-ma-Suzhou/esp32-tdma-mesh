idf_component_register(
    SRCS "src/sensor_batt.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc
)
