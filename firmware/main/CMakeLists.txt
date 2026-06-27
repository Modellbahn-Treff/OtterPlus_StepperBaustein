idf_component_register(
    SRCS "main.cpp" "stepper.cpp" "display.cpp" "settings.cpp" "serial_config.cpp" "gpio_inputs.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_driver_gpio esp_event esp_netif log freertos
             espressif__cjson esp_driver_uart esp_driver_i2c esp_timer driver
)
