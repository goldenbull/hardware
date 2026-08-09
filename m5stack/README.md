# M5Stack Basic 时钟与温湿度显示

适用于 M5Stack Basic Core v2.7 + Base BTC（SHT30）。工程使用 PlatformIO、Arduino 和 M5Unified。

## 准备环境

在 VS Code 中安装 `PlatformIO IDE` 扩展，然后用 VS Code 打开本目录。PlatformIO 会根据 `platformio.ini` 自动安装固定版本的 ESP32 平台和 M5Unified。

## 配置

启动时读取 TF 卡根目录下的 `clock-config.ini`，改配置不需要重新编译。把仓库里的 `clock-config.example.ini` 复制到卡上重命名即可：

```ini
wifi_ssid = your-ssid
wifi_password = your-password
ntp_server = ntp.aliyun.com
timezone = CST-8
cold_threshold = 20
hot_threshold = 30
```

格式为每行 `key = value`，`#` 或 `;` 开头是注释，`[section]` 行会被忽略（键名是平的，分组只是给人看的）。值两端的空格会被去掉，密码本身首尾带空格时用引号包起来。`cold_threshold` 必须小于 `hot_threshold`，否则两项一起回退到默认值。

没插卡、没有该文件、或文件里缺某一项时，对应的值退回编译时的默认值——即 `include/secrets.h`（已被 Git 忽略，可从 `include/secrets.example.h` 复制）和 `include/app_config.h` 里的 `APP_*` 宏。所以卡是可选的，出厂默认烧一份进固件、现场用卡改配置这两种用法都成立。

TF 卡与 LCD 共用 SPI 总线，因此只在启动时挂载一次，读完立即卸载，运行期间不占用总线。串口会打印实际生效的配置。

程序启动时会自动检测 I2C 地址 `0x44` 上的 SHT30。传感器未连接时，程序每五秒低频重试；运行中连接或断开传感器均不需要重新编译或重启。

## 编译和烧录

使用 VS Code 底部 PlatformIO 工具栏的 Build、Upload 和 Serial Monitor 按钮。也可使用 PlatformIO CLI：

```bash
pio run
pio run --target upload
pio device monitor
```

串口端口由 PlatformIO 自动探测。波特率已在 `platformio.ini` 中设为 `monitor_speed = 115200`，与固件里的 `Serial.begin(115200)` 一致；不设这一项时 PlatformIO 默认用 9600，串口监视器会输出乱码。

Wi-Fi 连接和 NTP 校时在后台任务里进行，`setup()` 不等待，因此上电后立即出画面。校时成功前时间从 epoch 开始走，屏幕左上角显示橙色“未校时”，同步完成后自动消失。连不上时后台每十秒重试一次；同步成功后每小时校一次漂移。

连接 SHT30 后，温度高于 `hot_threshold` 显示火焰动画，低于 `cold_threshold` 显示雪花动画。

## 按键

正面三个按键从左到右：

| 按键 | 功能 |
| --- | --- |
| A | 开关屏幕背光 |
| B | 调暗 |
| C | 调亮 |

亮度共五档，按住 B 或 C 可连续调节。调节时左上角短暂显示太阳图标和档位条，1.5 秒后自动消失。

最暗一档仍然可见（PWM 占空比 16/255），关屏只能用 Button A。背光关闭时 B 和 C 无效，避免在黑屏状态下误以为设备失灵。档位表在 `src/main.cpp` 的 `kBrightnessLevels` 中，可按需修改。亮度不写入 NVS，重启后回到默认的第 3 档。

## 电量显示

屏幕右上角显示电池电量和充电状态，每十秒刷新一次：

- 电量百分比和电池图标。大于 50% 为绿色，26%–50% 为橙色，不大于 25% 为红色。
- 正在充电时，图标左侧出现黄色闪电；充满后闪电变为绿色。
- 未检测到电源管理芯片时显示灰色 `--`。

电量来自 Core Basic 上地址为 `0x75` 的 IP5306 电源管理芯片，与 SHT30 共用 I2C 总线。IP5306 只能读出四颗充电指示灯的状态，所以电量只有 0/25/50/75/100 五档；Core Basic 没有电池电压 ADC，无法得到更精细的读数。
