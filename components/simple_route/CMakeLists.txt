idf_component_register(
    SRCS "src/simple_route.c"
    INCLUDE_DIRS "include"
    REQUIRES espnow_core esp_timer freertos json
)