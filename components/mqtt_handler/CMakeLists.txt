idf_component_register(
    SRCS "src/mqtt_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt
)