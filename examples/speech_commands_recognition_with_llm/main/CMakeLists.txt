set(requires
    driver
    esp_driver_i2s
    esp_timer
    nvs_flash
    esp_wifi
    esp_netif
    esp_websocket_client
    mbedtls
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       audio_manager.cc
                       wifi_manager.cc
                       websocket_client.cc
                       INCLUDE_DIRS
                       "."
                       REQUIRES ${requires}
                       )

