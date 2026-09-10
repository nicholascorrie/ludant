idf_component_register(
    SRCS
        "main.cpp"
        "ble_ota_server.cpp"
        "ota_manager.cpp"
        "ota_permission.cpp"
        "device_info.cpp"
        "module_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update bt driver esp_timer json mbedtls nvs_flash
)
