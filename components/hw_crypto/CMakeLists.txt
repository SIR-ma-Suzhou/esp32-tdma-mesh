idf_component_register(
    SRCS "src/hw_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls esp_hw_support esp_system esp_timer
)