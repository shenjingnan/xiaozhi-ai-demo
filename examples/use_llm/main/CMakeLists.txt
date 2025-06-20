set(requires
    driver
    esp_driver_i2s
    esp_timer
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       INCLUDE_DIRS
                       "."
                       )

