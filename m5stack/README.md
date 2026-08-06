# M5Stack Basic 时钟与温湿度显示

适用于 M5Stack Basic Core v2.7 + Base BTC（SHT30）。工程使用 PlatformIO、Arduino 和 M5Unified。

## 准备环境

在 VS Code 中安装 `PlatformIO IDE` 扩展，然后用 VS Code 打开本目录。PlatformIO 会根据 `platformio.ini` 自动安装固定版本的 ESP32 平台和 M5Unified。

## 配置

本地 Wi-Fi 配置位于 `include/secrets.h`，该文件已被 Git 忽略。新环境可复制 `include/secrets.example.h` 并填写：

```cpp
#define APP_WIFI_SSID "your-ssid"
#define APP_WIFI_PASSWORD "your-password"
```

SHT30 默认关闭，因此未连接传感器时不会访问 I2C 或重复输出错误。连接 Base BTC 后，在 `include/secrets.h` 中增加：

```cpp
#define APP_ENABLE_SHT30 1
```

NTP 服务器和时区可在同一文件中覆盖：

```cpp
#define APP_NTP_SERVER "ntp.aliyun.com"
#define APP_TIMEZONE "CST-8"
```

## 编译和烧录

使用 VS Code 底部 PlatformIO 工具栏的 Build、Upload 和 Serial Monitor 按钮。也可使用 PlatformIO CLI：

```bash
pio run
pio run --target upload
pio device monitor
```

串口已在 `platformio.ini` 中配置为 `/dev/cu.usbserial-01DB74DA`。如果设备端口变化，请同时修改 `upload_port` 和 `monitor_port`。

程序连接 Wi-Fi 后通过 NTP 校时，并每小时重新同步。正面最左侧 Button A 切换屏幕背光。连接 SHT30 后，温度高于 30°C 时显示火焰动画，低于 20°C 时显示雪花动画。
