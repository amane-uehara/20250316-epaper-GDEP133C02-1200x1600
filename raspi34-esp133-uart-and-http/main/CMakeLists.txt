idf_component_register(
    SRCS "main.c" "wifi_manager.c" "http_server.c" "uart_receiver.c" "epd_driver.c" "dns_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_http_server esp_driver_spi esp_driver_gpio esp_driver_uart nvs_flash esp_netif esp_event
)
