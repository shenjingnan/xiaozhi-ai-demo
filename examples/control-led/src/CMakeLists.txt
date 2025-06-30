# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(SRCS ${app_sources})

# 为 ESP-IDF 直接构建提供支持
if(NOT PLATFORMIO)
    set_property(GLOBAL APPEND PROPERTY __PROJECT_COMPONENT_REQUIRES src)
endif()
