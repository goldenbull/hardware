# AMOLED 桌面时钟

Waveshare **ESP32-S3-Touch-AMOLED-1.91**（536×240 SH8601 QSPI 屏 + FT3168 触摸）上的
桌面时钟，外接 Sensirion **SHT45** 读温湿度。ESP-IDF 工程，LVGL 8。

## 功能

| 功能 | 实现 |
| --- | --- |
| 时钟 | Wi-Fi 连接后走 NTP 校时，显示 `2026-08-07  Fri` + `14:23:05`，每秒刷新，每小时重新对时 |
| 温湿度 | I2C 上的 SHT45，每 2 秒一次高精度测量（带 CRC 校验），显示在时钟下方 |
| 冷热动画 | 温度 > 30°C 屏幕底部烧火，< 20°C 顶部飘雪，20~30°C 之间不显示动画 |
| 触摸 | 单击开关屏幕；横向滑动调亮度，屏幕中央出现亮度浮层，松手 1.2 秒后消失 |
| 无闪烁 | 见下面「关于刷新」 |

阈值可以在 `idf.py menuconfig` → `AMOLED Clock 配置` 里改。

## 接线

SHT45 挂在板子已有的 I2C0 上，和触摸（0x38）、IMU（0x6B）共用，地址 0x44 不冲突。

| SHT45 线色 | 接到 | 板上引脚 |
| --- | --- | --- |
| 红 | 3V3 | 3V3 |
| 黑 | GND | GND |
| 黄（SDA） | I2C 数据 | GPIO40 |
| 绿（SCL） | I2C 时钟 | GPIO39 |

屏蔽层接 GND。板上已经有上拉电阻，传感器那头不用再加。

总线速率设成了 200kHz（`main/board_pins.h` 里的 `I2C_CLK_SPEED_HZ`）。官方例程用的是
300kHz，这里降了一档是因为 SHT45 是带线引出的，线越长总线电容越大。如果线超过 50cm
读数不稳（日志里出现 CRC 失败），继续降到 100000。

## 编译烧录

需要 ESP-IDF **5.2 以上**（用了新版 `driver/i2c_master.h`；SH8601 组件另外要求
`>5.0.4` 且 `!=5.1.1`）。在 IDF 6.0.x 上开发验证。

Windows 下先进 IDF 环境，再在工程目录里操作：

```bat
C:\esp\v6.0.2\esp-idf\export.bat
cd /d D:\src\hardware\amoled-clock
idf.py set-target esp32s3
idf.py menuconfig        :: 填 Wi-Fi SSID / 密码，按需改 NTP 服务器和温度阈值
idf.py build
idf.py -p COM6 flash monitor
```

`idf.py build` 会自动从组件仓库拉 `esp_lcd_sh8601` 和 `lvgl/lvgl`，第一次需要联网。

工程目录在 D 盘，WSL 和 Windows 看到的是同一份文件，但 **只在 Windows 侧编译**——
两边交替 build 会让 CMake 缓存里的绝对路径打架。

## 关于刷新（第 5 条需求）

LVGL 配了两块 **整屏大小** 的绘制缓冲（536×240×2 = 251KB 一块，都在 PSRAM），
并且打开了 `full_refresh`：

- 每一帧 LVGL 都在内存里把整屏合成好，再通过一次 `esp_lcd_panel_draw_bitmap`
  DMA 给屏幕。屏幕上不存在「先擦掉旧内容、再画新内容」的中间状态。
- 两块缓冲交替使用：一块正在 DMA 出去的时候，LVGL 已经在另一块上画下一帧了，
  不需要等传输完成。
- 没有任何东西变化时不会触发刷新，静止画面下 SPI 是空闲的。

相关代码在 `main/display.c` 的 `display_init()` 和 `lvgl_flush_cb()`。

另外 `lvgl_rounder_cb()` 把刷新窗口的边界对齐到偶数——这是 SH8601 的硬性要求，
去掉会花屏。

## 交互细节

- **单击**：手指位移不超过 20px 且按住不超过 500ms 才算单击，避免滑动时误触发。
- **横滑**：横向位移超过 32px 且竖向偏移小于 80px 才进入调节模式。划过 400px
  覆盖整个亮度范围，可以来回拖动微调。
- 亮度是往 SH8601 的 `0x51` 寄存器写 0~255（这块屏是 AMOLED，没有背光引脚）。
  最低档限制在 8，防止滑到全黑之后找不到屏幕；要彻底关屏用单击。
- **关屏状态下滑动无效**，只有单击能重新点亮。
- 关屏时时钟和动画的重绘都会跳过，不做无用功。

## 代码结构

```
main/
  app_main.c    启动流程、传感器任务、网络状态轮询
  board_pins.h  引脚定义（取自官方出厂固件）
  display.c     SH8601 + LVGL 初始化、亮度、开关屏
  touch.c       FT3168 轮询 + 手势判定
  sht4x.c       SHT40/41/45 驱动（三个型号命令集相同）
  net_time.c    Wi-Fi + SNTP
  ui.c          界面布局、时钟刷新、火焰/雪花动画、亮度浮层
  fonts/        中文字模（生成物）
tools/
  gen_fonts.sh  重新生成字模
```

## 中文字体

界面文字是中文的，字模在 `main/fonts/` 下，由 `lv_font_conv` 从 **更纱黑体 Fixed SC
Regular** 生成。这个字体是 SIL OFL 授权的，可以随固件分发；等宽的字形用在时钟上
数字不会跳动。

三份字模按用途裁剪，只包含实际会出现的字符：

| 文件 | 大小 | 内容 |
| --- | --- | --- |
| `font_sc_16.c` | 16px | ASCII + 22 个汉字 + `°`，右上角状态提示用 |
| `font_sc_28.c` | 28px | 同上，日期 / 温湿度 / 亮度用 |
| `font_sc_48.c` | 48px | 只有 `0-9` 和 `:`，大号时钟用 |

**改文案时如果用到了新的汉字，必须重新生成字模**，否则新字会显示成方框。把字补进
`tools/gen_fonts.sh` 里的 `CJK` 变量再跑：

```bash
bash tools/gen_fonts.sh                    # 用 repo 里的更纱黑体
bash tools/gen_fonts.sh /path/to/other.ttf # 换字体
```

脚本用 `npx` 拉 `lv_font_conv`，需要 node 和联网。

## 已知限制

- 字模是按当前文案裁剪的，加新字要重新生成，见上面「中文字体」一节。
- 用的是新版 I2C 驱动 `driver/i2c_master.h`。Waveshare 官方例程用的是 legacy 的
  `driver/i2c.h`，那套在 IDF 6.0 已经 EOL、7.0 会直接删掉，所以没有沿用——
  如果你从官方例程搬别的驱动过来（比如 QMI8658），也要一起改。
- 亮度不写 NVS，重启后回到默认值 180。
