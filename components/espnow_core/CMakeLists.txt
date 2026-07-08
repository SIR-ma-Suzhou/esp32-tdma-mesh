idf_component_register(
    SRCS "src/espnow_core.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp-now nvs_flash freertos esp_netif esp_event
)