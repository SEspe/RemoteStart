idf_component_register(
    SRCS
        "main.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_now
        nvs_flash
        driver
        esp_http_server
        esp_ota_ops
        esp_timer
        app_update
        log
)
