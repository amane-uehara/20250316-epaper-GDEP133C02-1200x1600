idf_component_register(
    SRCS "main.c" "uart_receiver.c" "epd_driver.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_driver_spi esp_driver_gpio
)
