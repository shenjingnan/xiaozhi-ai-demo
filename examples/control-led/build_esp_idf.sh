#!/bin/bash
# ESP-IDF 构建脚本

# 创建临时的 main 目录以支持 ESP-IDF 构建
if [ ! -d "main" ]; then
    echo "Creating temporary main directory for ESP-IDF build..."
    mkdir -p main
    cp src/main.cpp main/
    echo 'idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")' > main/CMakeLists.txt
fi

# 清理并构建
rm -rf build
idf.py set-target esp32s3
idf.py build

# 清理临时文件
echo "Cleaning up temporary files..."
rm -rf main