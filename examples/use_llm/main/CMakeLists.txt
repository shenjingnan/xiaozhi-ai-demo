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
                       wifi_manager.cc
                       wake_word_detector.cc
                       audio_recorder.cc
                       server_client.cc
                       led_controller.cc
                       INCLUDE_DIRS
                       "."
                       )

