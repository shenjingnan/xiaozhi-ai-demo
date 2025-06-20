set(requires
    driver
    esp_driver_i2s
    esp_timer
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    lwip
    mbedtls
    heap
    espressif__esp-sr
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       INCLUDE_DIRS
                       "."
                       )

