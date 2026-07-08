idf_component_register(
    SRCS "src/tdma_scheduler.c"
    INCLUDE_DIRS "include"
    REQUIRES time_sync espnow_core esp_timer freertos json
)