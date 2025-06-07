set(requires
    driver
    esp_driver_i2s
    esp_timer
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    nvs_flash
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       INCLUDE_DIRS
                       "."
                       )

