idf_component_register(SRCS "ble_ota_recovery.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nimble esp_ota bootloader_support esp_partition
                       PRIV_REQUIRES mbedtls)