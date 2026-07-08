idf_component_register(
    SRCS "src/time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES espnow_core esp_wifi esp_timer freertos
)