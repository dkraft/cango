idf_component_register(
    SRCS
        "app_main.c"
        "board.c"
        "led.c"
        "can_port.c"
        "storage.c"
        "session.c"
        "ctrl.c"
        "wifi_mode.c"
        "ble_mode.c"
    INCLUDE_DIRS "."
    REQUIRES common nvs_flash esp_wifi esp_http_server esp_event esp_netif bt driver fatfs sdmmc esp_timer vfs
)
