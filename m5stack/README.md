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

程序启动时会自动检测 I2C 地址 `0x44` 上的 SHT30。传感器未连接时，程序每五秒低频重试；运行中连接或断开传感器均不需要重新编译或重启。

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
