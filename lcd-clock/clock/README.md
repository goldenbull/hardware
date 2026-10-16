# LCD 桌面时钟

Waveshare **ESP32-C5-Touch-LCD-3.5** 上的桌面时钟，逻辑照搬 `../../amoled-clock`，
外接 Sensirion **SHT45** 读温湿度，另外支持声控开关屏（默认听到够响的声音就切换）。

## 功能

| 功能 | 实现 |
| --- | --- |
| 时钟 | Wi-Fi 连接后走 NTP 校时，显示 `2026-10-15 周四` + `14:23:05`，每秒刷新，每小时重新对时 |
| 温湿度 | 外接 SHT45，每 2 秒一次高精度测量（带 CRC 校验），显示在时钟下方。可以热插拔 |
| 冷热动画 | 温度 > 30°C 屏幕底部烧火，< 20°C 顶部飘雪，20~30°C 之间不显示动画 |
| 触摸 | 单击开关屏幕；上下滑动调背光亮度，左侧出现亮度条，松手 1.2 秒后消失 |
| 调试面板 | 从右往左划进入，从左往右划返回。显示运行时间、启动原因、编译时间、芯片温度、内存、Wi-Fi（SSID / 信号 / IP / 校时 / 断线次数）、SHT45 探测与读数统计、IMU 三轴读数、麦克风峰值。每秒刷新。面板上单击照样关屏，关屏后自动切回时钟，下次开屏看到的是时钟 |
| 自动翻转 | QMI8658 判断重力方向，屏幕倒着放时转 180° |
| 声控开关屏 | **默认关闭**，在 menuconfig 里勾上 `启用音量触发开关屏` 才生效。方式默认是**音量触发**：平滑后的音量升过 -45dB 就切换开 / 关，两次开关屏至少隔 2 秒。也可以换成关键词识别（还没实现），见下面「声控」 |

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

## 声控

menuconfig → `LCD Clock 配置` → `声控方式` 二选一。

### 音量触发（默认）

卧室很安静，用不着识别说的是什么：拍一下手、清一下嗓子，屏幕就在开 / 关之间切换。

- `voice.c` 每 20ms 算一次均方功率，做一阶指数平滑（时间常数 100ms），换成 dBFS（满幅 = 0dB）
- 平滑音量从下往上越过 **-45dB** 算一次响声，切换开关屏（实测值：增益 30dB，1 米左右正好）
- 两次开关屏之间至少隔 **2 秒**，从上一次开关屏完成算起，**单击开关屏也算**
- 触发后音量要先回落到阈值以下 6dB 才能再次触发，一阵持续的响声（一直在说话、放音乐）只切一次
- 手指离开屏幕 1 秒内的响声不理会，敲屏的声音不会把刚单击切过的屏又切回去
- 开机头 1 秒不触发（codec 上电会"噗"一下）

**默认不启用**：menuconfig → `LCD Clock 配置` → `启用音量触发开关屏` 勾上才会开关屏。
不勾时麦克风照样在算音量，调试面板"声控"一行显示 `-62dB/-45 未启用`，可以先看着读数把阈值调好再打开。

阈值、平滑时间常数、间隔都在 menuconfig 里调。**调阈值看调试面板**的"声控"一行：
`-62dB/-45 触发3` 是最近 1 秒平滑音量的最大值 / 阈值 / 开机以来的触发次数（包括被间隔挡掉的）。
平时安静时的读数和想要触发的声音的读数，阈值取在两者之间。
注意这是均方根，比上一行的"峰值"小，说话时一般差 10dB 左右。

### 关键词识别（待实现）

采集链路已经接好：`voice.c` 的任务每 20ms 从 `components/mic` 读一块 16kHz/16bit 单声道 PCM，
喂给 `kws_feed()`。麦克风、增益和 `kws-recorder` 录训练数据时完全是同一份代码和配置
（menuconfig → `麦克风`，两个工程必须一致）。

**待实现的只有 `main/kws.c`**：`kws_init()` 加载模型，`kws_feed()` 返回 `KWS_SCREEN_ON` /
`KWS_SCREEN_OFF`，去抖也在里面做。返回结果之后的开关屏已经接好了（`app_main.c` 的 `on_voice`）。
关键词只"开"或"关"，不是切换，屏已经是目标状态时不做任何事。

## 代码结构

```
main/
  app_main.c    启动流程、传感器任务、网络状态轮询、声控回调（开关屏间隔在这里判断）
  gesture.c     单击开关屏、上下滑动调亮度、左右滑动切调试面板（触摸读取在 components/display/touch.c）
  debug_panel.c 调试面板，独立的 LVGL screen
  imu.c         QMI8658，自动翻转
  net_time.c    Wi-Fi + SNTP（和 amoled-clock 相同）
  ui.c          界面布局、时钟刷新、火焰/雪花动画、亮度条
  voice.c       麦克风采集任务、音量平滑和触发
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
