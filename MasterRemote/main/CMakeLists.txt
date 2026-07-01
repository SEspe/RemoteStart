idf_component_register(
    SRCS
        "main.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
nvs_flash
        esp_driver_gpio
        esp_http_server
esp_timer
        app_update
        log
)
