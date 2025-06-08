# WiFi 配置说明

## 配置WiFi连接

在使用本项目之前，您需要配置WiFi连接参数。

### 方法1：修改源代码（临时方案）

编辑 `main/main.cc` 文件，找到以下行：

```cpp
// WiFi配置
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
```

将 `your_wifi_ssid` 替换为您的WiFi网络名称，将 `your_wifi_password` 替换为您的WiFi密码。

### 方法2：使用menuconfig配置（推荐）

运行以下命令打开配置菜单：

```bash
idf.py menuconfig
```

然后导航到：
`Component config` -> `Example Configuration` -> `WiFi Configuration`

设置您的WiFi SSID和密码。

## 服务器配置

同时需要配置Python服务端的URL地址。在 `main/main.cc` 中找到：

```cpp
// 服务器配置
#define SERVER_URL "http://192.168.1.100:8080/process_audio"
```

将IP地址 `192.168.1.100` 替换为运行Python服务端的实际IP地址。

## 编译和烧录

配置完成后，执行以下命令编译和烧录：

```bash
# 编译
idf.py build

# 烧录（需要连接ESP32-S3开发板）
idf.py flash

# 查看串口输出
idf.py monitor
```

## 注意事项

1. 确保ESP32-S3和Python服务端在同一个网络中
2. 确保Python服务端已经启动并监听在8080端口
3. 如果遇到网络连接问题，检查防火墙设置
