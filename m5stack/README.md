# M5Stack Basic 时钟与温湿度显示

适用于 M5Stack Basic Core v2.7 + Base BTC（SHT30）。工程使用 ESP-IDF、C++ 和 CMake。

## 准备环境

安装 ESP-IDF 5.1 或更新版本并进入其命令行环境。首次配置/构建时，ESP-IDF Component Manager 会自动下载 Arduino-ESP32 与 M5Unified 依赖。

## 配置和编译

Wi-Fi SSID、密码、NTP 地址均通过 CMake 缓存参数传入：

```bash
idf.py set-target esp32
idf.py -DWIFI_SSID="你的SSID" \
       -DWIFI_PASSWORD="你的密码" \
       -DNTP_SERVER="ntp.aliyun.com" \
       -DTIMEZONE="CST-8" \
       build
```

烧录并查看日志（请按实际串口修改）：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

程序启动后连接 Wi-Fi 并等待 NTP 校时，SNTP 每小时重新同步一次。正面最左侧 Button A 切换屏幕背光；屏幕关闭时，校时和温湿度采集仍会继续。温度高于 30°C 时屏幕底部显示火焰动画，低于 20°C 时屏幕顶部显示飘落雪花动画；温度恰好等于阈值时不显示动画。

> CMake 参数会写入构建产物。若固件需要公开发布，请不要在其中放入真实 Wi-Fi 密码。
