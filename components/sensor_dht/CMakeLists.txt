idf_component_register(
    SRCS "src/sensor_dht.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer
)