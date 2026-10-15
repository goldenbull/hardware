# LCD 桌面时钟

Waveshare **ESP32-C5-Touch-LCD-3.5** 上的桌面时钟，逻辑照搬 `../../amoled-clock`，
外接 Sensirion **SHT45** 读温湿度，另外支持说"打开屏幕""关闭屏幕"开关屏。

## 功能

| 功能 | 实现 |
| --- | --- |
| 时钟 | Wi-Fi 连接后走 NTP 校时，显示 `2026-10-15 周四` + `14:23:05`，每秒刷新，每小时重新对时 |
| 温湿度 | 外接 SHT45，每 2 秒一次高精度测量（带 CRC 校验），显示在时钟下方。可以热插拔 |
| 冷热动画 | 温度 > 30°C 屏幕底部烧火，< 20°C 顶部飘雪，20~30°C 之间不显示动画 |
| 触摸 | 单击开关屏幕；上下滑动调背光亮度，左侧出现亮度条，松手 1.2 秒后消失 |
| 自动翻转 | QMI8658 判断重力方向，屏幕倒着放时转 180° |
| 语音开关屏 | 麦克风一直在采集，**识别部分还没实现**，见下面「语音识别」 |

阈值、Wi-Fi、亮度等在 `idf.py menuconfig` → `LCD Clock 配置` 里改。

和 amoled-clock 不一样的地方：

- 屏是 320x480 竖装的，这里设成**横屏 480x320**（ST7796 的 MADCTL 交换行列，不花 CPU）。
  比 536x240 窄一点、高很多，所以日期、时钟、温湿度分三行，字号 40 / 96 / 40
- 背光走 CH32V003 的 PWM（0~100%），不是 AMOLED 的亮度寄存器。关屏 = 背光灭 + DISPOFF
- 板载 SHTC3 不用，它被板子自己的发热带偏（`../sensor-test` 测出来的）

## 接线

SHT45 挂在板上的 I2C0（SCL=GPIO26，SDA=GPIO27），地址 0x44，和板上器件都不冲突，见 `../README.md`。

- VDD 默认接板上 **3V3**。想像 amoled-clock 那样用 GPIO 供电，在 menuconfig 里填 `SHT45 供电 GPIO`
- **传感器要用线引到板子外面**，别贴着板子或者塞进外壳，不然又会跟 SHTC3 一样被加热
- 总线跑 400kHz。线长、读数不稳（日志里出现 CRC 失败）时，把 `components/board/board_pins.h`
  的 `I2C_CLK_SPEED_HZ` 降到 200000 或 100000

## 编译烧录

```
source ~/.espressif/tools/activate_idf_v6.1.sh
cd ~/src/hardware/lcd-clock/clock
idf.py menuconfig        # 填 Wi-Fi SSID / 密码
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## 上机要确认的两件事

这两处没在硬件上实测过：

1. **IMU 的轴**：默认用 Y 轴（X 轴试过不对）。开机日志会打印 `QMI8658 在线 ... ax= ay= az=`。把板子横着立起来，读数接近 ±1g
   的轴就是 menuconfig 里 `IMU 上哪个轴对着屏幕的上下方向` 要选的；立正了却显示倒的，勾上
   `IMU 方向取反`。实在不需要就关掉 `根据 IMU 自动把屏幕翻转 180°`
2. **横屏下的触摸方向**：坐标换算在 `components/display/display.c` 的 `display_map_touch()`，
   按 MADCTL 推出来的。往上滑应该变亮；方向反了改那里

## 语音识别

采集链路已经接好：`voice.c` 的任务每 20ms 从 `components/mic` 读一块 16kHz/16bit 单声道 PCM，
喂给 `kws_feed()`。麦克风、增益和 `kws-recorder` 录训练数据时完全是同一份代码和配置
（menuconfig → `麦克风`，两个工程必须一致）。

**待实现的只有 `main/kws.c`**：`kws_init()` 加载模型，`kws_feed()` 返回 `KWS_SCREEN_ON` /
`KWS_SCREEN_OFF`，去抖也在里面做。返回结果之后的开关屏已经接好了（`app_main.c` 的 `on_keyword`）。
语音只"开"或"关"，不是切换，屏已经是目标状态时不做任何事。

## 代码结构

```
main/
  app_main.c    启动流程、传感器任务、网络状态轮询、语音回调
  gesture.c     单击开关屏、上下滑动调亮度（触摸读取在 components/display/touch.c）
  imu.c         QMI8658，自动翻转
  net_time.c    Wi-Fi + SNTP（和 amoled-clock 相同）
  ui.c          界面布局、时钟刷新、火焰/雪花动画、亮度条
  voice.c       麦克风采集任务
  kws.c         关键词识别（待实现）
  fonts/        字模（生成物）
tools/
  gen_fonts.sh  重新生成字模
```

共用组件：`board`（引脚、I2C、IO 扩展）、`display`（屏、触摸）、`mic`、`sht4x`。

## 字体

和 amoled-clock 同一套：更纱黑体 Fixed SC 和 DSEG7 Classic Bold，都是 SIL OFL 授权。

| 文件 | 内容 |
| --- | --- |
| `font_sc_16.c` | ASCII + 界面上的汉字 + `°`，右上角状态提示用 |
| `font_sc_40.c` | ASCII + `°` + 星期 + 传感器离线，日期行 / 温湿度行 |
| `font_seg_96.c` | DSEG7，只有 `0-9` 和 `:`，大号时钟 |

**改文案用到新汉字时必须重新生成字模**：把字补进 `tools/gen_fonts.sh` 的 `CJK` 再跑
`bash tools/gen_fonts.sh`（需要 node 和联网）。

## 已知限制

- 亮度不写 NVS，重启后回到 menuconfig 里的默认值
- 板上有 PCF85063 RTC 但没用，断网重启后要等 NTP 校时才有正确时间
