idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES espnow_core time_sync tdma_scheduler simple_route hw_crypto json_converter mqtt_handler sensor_dht sensor_pir sensor_batt topology_aggregation bt json
)
