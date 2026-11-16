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
        "gvret.c"
    INCLUDE_DIRS "."
    REQUIRES common nvs_flash esp_wifi esp_http_server esp_event esp_netif bt driver fatfs sdmmc esp_timer vfs lwip
)

# ffconf.h keeps exFAT off unless this wins the #ifndef. The field card is exFAT.
idf_component_get_property(fatfs_lib fatfs COMPONENT_LIB)
target_compile_definitions(${fatfs_lib} PUBLIC FF_FS_EXFAT=1)
