# lcd-clock

Waveshare [ESP32-C5-Touch-LCD-3.5](https://docs.waveshare.net/ESP32-C5-Touch-LCD-3.5/) 上的几个平行的
ESP-IDF 工程，共用 `components/` 里的板级驱动。

| 目录 | 内容 |
| --- | --- |
| `sensor-test/` | 温湿度传感器测试。结论：板载 SHTC3 随板子发热持续升高，不能用，改外接 SHT45 |
| `kws-recorder/` | 用板载麦克风录"打开屏幕""关闭屏幕"，存成 WAV 到 TF 卡，作为语音识别的训练数据 |
| `clock/` | 最终的固件：时钟、温湿度、冷热动画、触摸手势、语音开关屏（识别部分待实现），逻辑照搬 amoled-clock |
| `components/board` | 引脚表 `board_pins.h`、I2C0 总线、CH32V003 IO 扩展（屏/触摸复位、功放、背光） |
| `components/display` | ST7796 屏（竖屏 / 横屏、180° 翻转、开关屏、背光）+ FT6336 触摸，接成 LVGL 8 |
| `components/sdcard` | TF 卡（SPI 模式，和屏共用 SPI2） |
| `components/mic` | I2S0 + ES8311 麦克风。录音和识别必须用同一份，增益在 menuconfig → `麦克风` |
| `components/sht4x` | 外接 SHT40/41/45 驱动，来自 amoled-clock |

每个工程在 `CMakeLists.txt` 的 `EXTRA_COMPONENT_DIRS` 里只列自己用到的组件。

需要 ESP-IDF **6.1**：

```
source ~/.espressif/tools/activate_idf_v6.1.sh
cd ~/src/hardware/lcd-clock/<工程目录>
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## I2C0 地址

SCL=GPIO26，SDA=GPIO27，400kHz。

| 器件 | 地址 |
| --- | --- |
| ES8311 音频 | 0x18 |
| CH32V003 IO 扩展 | 0x24 |
| AXP2101 电源 | 0x34 |
| FT6336 触摸 | 0x38 |
| 外接 SHT45 | 0x44 |
| PCF85063 RTC | 0x51 |
| QMI8658 IMU | 0x6B |
| 板载 SHTC3 | 0x70 |
