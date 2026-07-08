idf_component_register(
    SRCS "src/topology_aggregation.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer json tdma_scheduler espnow_core
)
