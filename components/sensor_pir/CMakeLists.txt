idf_component_register(
    SRCS "src/sensor_pir.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_hw_support esp_timer
)
