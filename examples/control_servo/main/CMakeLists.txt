set(requires
    driver
    esp_driver_i2s
    esp_timer
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       servo_controller.cc
                       INCLUDE_DIRS
                       "."
                       )

