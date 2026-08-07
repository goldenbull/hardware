#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "app_config.h"

namespace
{
constexpr uint8_t kSht30Address = 0x44;
// Core Basic v2.7 carries an IP5306 power-management IC strapped for I2C.
constexpr uint8_t  kIp5306Address         = 0x75;
constexpr uint8_t  kIp5306RegCharge       = 0x70;
constexpr uint8_t  kIp5306RegChargeFull   = 0x71;
constexpr uint8_t  kIp5306RegBatteryLeds  = 0x78;
constexpr uint32_t kSensorIntervalMs      = 2000;
constexpr uint32_t kSensorProbeIntervalMs = 5000;
constexpr uint32_t kBatteryIntervalMs     = 10000;
constexpr uint32_t kNtpSyncIntervalMs     = 60U * 60U * 1000U;
constexpr uint32_t kAnimationIntervalMs   = 50;
constexpr uint32_t kWifiTimeoutMs         = 20000;
constexpr uint32_t kBrightnessOverlayMs   = 1500;
constexpr uint32_t kButtonRepeatDelayMs   = 500;
constexpr uint32_t kButtonRepeatRateMs    = 150;

// Backlight duty ladder, spaced roughly geometrically so the steps look even.
// The first entry is the dimmest setting and is deliberately non-zero: the
// screen stays readable in the dark, only Button A turns the backlight off.
constexpr uint8_t kBrightnessLevels[] = {16, 24, 36, 52, 76, 104, 128, 160, 200, 255};
constexpr int     kBrightnessCount    = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);
constexpr int     kBrightnessDefault  = 6; // 128, the level used before this was adjustable

M5Canvas canvas(&M5.Display);
bool     screen_on           = true;
bool     display_ok          = false;
bool     sensor_present      = false;
bool     sensor_ok           = false;
float    temperature         = NAN;
float    humidity            = NAN;
bool     battery_present     = false;
int      battery_level       = 0;
bool     battery_charging    = false;
bool     battery_full        = false;
uint32_t last_sensor_read    = 0;
uint32_t last_sensor_probe   = 0;
uint32_t last_battery_read   = 0;
uint32_t last_animation_draw = 0;
uint32_t animation_frame     = 0;
int      last_drawn_second   = -1;
int      last_animation_mode = 0;
bool     force_redraw        = true;
bool     environment_dirty   = true;

int      brightness_index         = kBrightnessDefault;
bool     brightness_overlay_shown = false;
uint32_t brightness_overlay_until = 0;
uint32_t dim_repeat_at            = 0;
uint32_t brighten_repeat_at       = 0;

uint8_t crc8(const uint8_t* data, size_t size)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

bool detectSht30()
{
    Wire.beginTransmission(kSht30Address);
    return Wire.endTransmission() == 0;
}

bool readSht30(float& temp, float& rh)
{
    Wire.beginTransmission(kSht30Address);
    Wire.write(0x2C);
    Wire.write(0x06);
    if (Wire.endTransmission() != 0)
        return false;
    delay(20);

    if (Wire.requestFrom(kSht30Address, static_cast<uint8_t>(6)) != 6)
        return false;
    uint8_t data[6];
    for (auto& byte : data)
        byte = Wire.read();
    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5])
        return false;

    const uint16_t raw_temp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    const uint16_t raw_rh   = (static_cast<uint16_t>(data[3]) << 8) | data[4];
    temp                    = -45.0f + 175.0f * raw_temp / 65535.0f;
    rh                      = 100.0f * raw_rh / 65535.0f;
    return true;
}

bool readIp5306(uint8_t reg, uint8_t& value)
{
    Wire.beginTransmission(kIp5306Address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom(kIp5306Address, static_cast<uint8_t>(1)) != 1)
        return false;
    value = Wire.read();
    return true;
}

// The IP5306 only exposes the four charge LEDs, so the level is quantised to
// 25% steps. There is no battery-voltage ADC on the Core Basic.
void readBattery()
{
    uint8_t leds   = 0;
    uint8_t charge = 0;
    uint8_t full   = 0;
    if (!readIp5306(kIp5306RegBatteryLeds, leds)
        || !readIp5306(kIp5306RegCharge, charge)
        || !readIp5306(kIp5306RegChargeFull, full))
    {
        if (battery_present)
            Serial.println("IP5306 power IC not responding");
        battery_present  = false;
        battery_level    = 0;
        battery_charging = false;
        battery_full     = false;
        return;
    }

    battery_present = true;
    switch (leds & 0xF0)
    {
    case 0xE0:
        battery_level = 25;
        break;
    case 0xC0:
        battery_level = 50;
        break;
    case 0x80:
        battery_level = 75;
        break;
    case 0x00:
        battery_level = 100;
        break;
    default:
        battery_level = 0;
        break;
    }
    battery_charging = (charge & 0x08) != 0;
    battery_full     = (full & 0x08) != 0;
    if (battery_full)
        battery_level = 100;
}

void applyBrightness()
{
    if (!display_ok)
        return;
    M5.Display.setBrightness(screen_on ? kBrightnessLevels[brightness_index] : 0);
}

void adjustBrightness(int delta)
{
    const int next = brightness_index + delta;
    if (next < 0 || next >= kBrightnessCount)
        return;
    brightness_index = next;
    applyBrightness();
    Serial.printf("Brightness step %d/%d (duty %u)\n",
                  brightness_index + 1,
                  kBrightnessCount,
                  static_cast<unsigned>(kBrightnessLevels[brightness_index]));
}

// True on the initial press, then repeatedly while the button stays held.
bool buttonRepeat(m5::Button_Class& button, uint32_t& repeat_at, uint32_t now_ms)
{
    if (button.wasPressed())
    {
        repeat_at = now_ms + kButtonRepeatDelayMs;
        return true;
    }
    if (button.isPressed() && static_cast<int32_t>(now_ms - repeat_at) >= 0)
    {
        repeat_at = now_ms + kButtonRepeatRateMs;
        return true;
    }
    return false;
}

void showStatus(const char* line1, const char* line2 = nullptr)
{
    if (!display_ok)
        return;

    canvas.fillScreen(TFT_BLACK);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setFont(&fonts::efontCN_24);
    canvas.drawString(line1, 160, line2 ? 100 : 120);
    if (line2)
        canvas.drawString(line2, 160, 140);
    canvas.pushSprite(0, 0);
}

bool connectWifi()
{
    if (APP_WIFI_SSID[0] == '\0')
    {
        showStatus("未设置 Wi-Fi");
        Serial.println("Wi-Fi credentials are not configured");
        return false;
    }

    showStatus("正在连接 Wi-Fi...");
    Serial.printf("Connecting to Wi-Fi SSID: %s\n", APP_WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);

    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < kWifiTimeoutMs)
    {
        M5.update();
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        showStatus("Wi-Fi 连接失败");
        Serial.printf("Wi-Fi connection failed, status=%d\n", WiFi.status());
        return false;
    }

    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

void startClockSync()
{
    showStatus("正在校准时间...");
    setenv("TZ", APP_TIMEZONE, 1);
    tzset();
    esp_sntp_set_sync_interval(kNtpSyncIntervalMs);
    configTzTime(APP_TIMEZONE, APP_NTP_SERVER);

    tm now{};
    for (int attempt = 0; attempt < 30 && !getLocalTime(&now, 1000); ++attempt)
        M5.update();
}

void drawSnow(uint32_t frame)
{
    for (int i = 0; i < 14; ++i)
    {
        const int x      = (i * 47 + 19) % 320;
        const int speed  = 1 + i % 3;
        const int y      = (static_cast<int>(frame) * speed + i * 17) % 48;
        const int radius = 1 + i % 2;
        canvas.drawLine(x - radius, y, x + radius, y, TFT_WHITE);
        canvas.drawLine(x, y - radius, x, y + radius, TFT_WHITE);
        if (radius == 2)
        {
            canvas.drawPixel(x - 1, y - 1, TFT_CYAN);
            canvas.drawPixel(x + 1, y + 1, TFT_CYAN);
        }
    }
}

void drawFire(uint32_t frame)
{
    constexpr int kBottom     = 239;
    constexpr int kFlameCount = 13;
    for (int i = 0; i < kFlameCount; ++i)
    {
        const int   center_x     = i * 26 - 4;
        const float wave         = sinf(frame * 0.28f + i * 1.37f);
        const int   outer_height = 16 + (i * 7) % 10 + static_cast<int>(wave * 4);
        const int   inner_height = outer_height * 2 / 3;

        canvas.fillTriangle(center_x - 16,
                            kBottom,
                            center_x + 16,
                            kBottom,
                            center_x + static_cast<int>(wave * 5),
                            kBottom - outer_height,
                            TFT_RED);
        canvas.fillTriangle(center_x - 10,
                            kBottom,
                            center_x + 10,
                            kBottom,
                            center_x - static_cast<int>(wave * 3),
                            kBottom - inner_height,
                            TFT_ORANGE);
        canvas.fillCircle(center_x, kBottom - 4, 6, TFT_YELLOW);
    }
}

int getAnimationMode()
{
    if (!sensor_ok)
        return 0;
    if (temperature < 20.0f)
        return -1;
    if (temperature > 30.0f)
        return 1;
    return 0;
}

// Battery gauge in the top-right corner: "<level>% [bolt] [icon]".
void drawBattery()
{
    constexpr int kIconTop    = 6;
    constexpr int kIconWidth  = 32;
    constexpr int kIconHeight = 16;
    constexpr int kIconRight  = 314; // the terminal nub reaches x = 316
    constexpr int kIconLeft   = kIconRight - kIconWidth;

    uint16_t color = TFT_DARKGREY;
    if (battery_present)
    {
        if (battery_level <= 25)
            color = TFT_RED;
        else if (battery_level <= 50)
            color = TFT_ORANGE;
        else
            color = TFT_GREEN;
    }

    canvas.drawRect(kIconLeft, kIconTop, kIconWidth, kIconHeight, color);
    canvas.fillRect(kIconRight, kIconTop + 5, 3, 6, color);
    if (battery_present && battery_level > 0)
    {
        const int fill = (kIconWidth - 4) * battery_level / 100;
        if (fill > 0)
            canvas.fillRect(kIconLeft + 2, kIconTop + 2, fill, kIconHeight - 4, color);
    }

    int text_right = kIconLeft - 4;
    if (battery_present && battery_charging)
    {
        const uint16_t bolt = battery_full ? TFT_GREEN : TFT_YELLOW;
        const int      x    = kIconLeft - 12;
        canvas.fillTriangle(x + 6, kIconTop, x, kIconTop + 10, x + 5, kIconTop + 10, bolt);
        canvas.fillTriangle(x + 2, kIconTop + 16, x + 8, kIconTop + 6, x + 3, kIconTop + 6, bolt);
        text_right = x - 4;
    }

    char label[8];
    if (battery_present)
        snprintf(label, sizeof(label), "%d%%", battery_level);
    else
        snprintf(label, sizeof(label), "--");
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(color, TFT_BLACK);
    canvas.drawRightString(label, text_right, kIconTop);
}

// Transient brightness readout in the top-left corner: a sun and a step bar.
void drawBrightnessOverlay()
{
    constexpr int kSunX          = 14;
    constexpr int kSunY          = 14;
    constexpr int kSunRadius     = 4;
    constexpr int kSegmentLeft   = 26;
    constexpr int kSegmentTop    = 10;
    constexpr int kSegmentWidth  = 6;
    constexpr int kSegmentHeight = 8;
    constexpr int kSegmentPitch  = 8;

    canvas.fillCircle(kSunX, kSunY, kSunRadius, TFT_YELLOW);
    canvas.drawLine(kSunX, kSunY - 9, kSunX, kSunY - 7, TFT_YELLOW);
    canvas.drawLine(kSunX, kSunY + 7, kSunX, kSunY + 9, TFT_YELLOW);
    canvas.drawLine(kSunX - 9, kSunY, kSunX - 7, kSunY, TFT_YELLOW);
    canvas.drawLine(kSunX + 7, kSunY, kSunX + 9, kSunY, TFT_YELLOW);

    for (int i = 0; i < kBrightnessCount; ++i)
    {
        const int x = kSegmentLeft + i * kSegmentPitch;
        if (i <= brightness_index)
            canvas.fillRect(x, kSegmentTop, kSegmentWidth, kSegmentHeight, TFT_YELLOW);
        else
            canvas.drawRect(x, kSegmentTop, kSegmentWidth, kSegmentHeight, TFT_DARKGREY);
    }
}

void drawScreen(const tm& now, int animation_mode)
{
    static const char* const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    char                     date[32];
    char                     clock[16];
    snprintf(date,
             sizeof(date),
             "%04d-%02d-%02d  %s",
             now.tm_year + 1900,
             now.tm_mon + 1,
             now.tm_mday,
             weekdays[now.tm_wday]);
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);

    canvas.fillScreen(TFT_BLACK);
    if (animation_mode < 0)
        drawSnow(animation_frame);
    else if (animation_mode > 0)
        drawFire(animation_frame);
    drawBattery();
    if (brightness_overlay_shown)
        drawBrightnessOverlay();

    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setFont(&fonts::efontCN_24);
    canvas.drawCenterString(date, 160, 48);

    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setFont(&fonts::Font7);
    canvas.drawCenterString(clock, 160, 86);

    char environment[64];
    if (sensor_ok)
        snprintf(environment, sizeof(environment), "温度 %.1f°C    湿度 %.1f%%", temperature, humidity);
    else
        snprintf(environment, sizeof(environment), "温湿度传感器不可用");
    canvas.setTextColor(sensor_ok ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
    canvas.setFont(&fonts::efontCN_24);
    canvas.drawCenterString(environment, 160, 180);

    canvas.pushSprite(0, 0);
    last_drawn_second   = now.tm_sec;
    last_animation_mode = animation_mode;
    environment_dirty   = false;
    force_redraw        = false;
}
} // namespace

void setup()
{
    Serial.begin(115200);
    Serial.println("M5Stack clock starting");

    auto config          = M5.config();
    config.clear_display = true;
    M5.begin(config);
    display_ok = M5.getDisplayCount() != 0;
    if (display_ok)
    {
        canvas.setColorDepth(8);
        display_ok = canvas.createSprite(320, 240) != nullptr;
    }
    Serial.printf("Display initialization: %s (board=%d, count=%u)\n",
                  display_ok ? "OK" : "FAILED",
                  static_cast<int>(M5.getBoard()),
                  static_cast<unsigned>(M5.getDisplayCount()));
    applyBrightness();

    Wire.begin(21, 22);
    sensor_present    = detectSht30();
    last_sensor_probe = millis();
    Serial.printf("SHT30 sensor: %s\n", sensor_present ? "detected" : "not detected");

    readBattery();
    last_battery_read = millis();
    Serial.printf("IP5306 power IC: %s\n", battery_present ? "detected" : "not detected");

    if (connectWifi())
        startClockSync();
}

void loop()
{
    M5.update();
    const uint32_t now_ms = millis();

    if (M5.BtnA.wasPressed())
    {
        screen_on = !screen_on;
        applyBrightness();
        if (screen_on)
            force_redraw = true;
    }

    // Button B dims, Button C brightens. Both are ignored while the backlight is
    // off so that Button A stays the only way back from a dark screen.
    if (screen_on)
    {
        const bool dim      = buttonRepeat(M5.BtnB, dim_repeat_at, now_ms);
        const bool brighten = buttonRepeat(M5.BtnC, brighten_repeat_at, now_ms);
        if (dim || brighten)
        {
            adjustBrightness(dim ? -1 : 1);
            brightness_overlay_shown = true;
            brightness_overlay_until = now_ms + kBrightnessOverlayMs;
            force_redraw             = true;
        }
    }

    if (brightness_overlay_shown && static_cast<int32_t>(now_ms - brightness_overlay_until) >= 0)
    {
        brightness_overlay_shown = false;
        force_redraw             = true;
    }

    if (!sensor_present && now_ms - last_sensor_probe >= kSensorProbeIntervalMs)
    {
        last_sensor_probe = now_ms;
        sensor_present    = detectSht30();
        if (sensor_present)
        {
            Serial.println("SHT30 sensor connected");
            last_sensor_read = 0;
        }
    }

    if (sensor_present && (now_ms - last_sensor_read >= kSensorIntervalMs || last_sensor_read == 0))
    {
        last_sensor_read = now_ms;
        if (!readSht30(temperature, humidity))
        {
            sensor_present    = false;
            sensor_ok         = false;
            temperature       = NAN;
            humidity          = NAN;
            last_sensor_probe = now_ms;
            Serial.println("SHT30 sensor disconnected");
        }
        else
        {
            sensor_ok = true;
        }
        environment_dirty = true;
    }

    if (now_ms - last_battery_read >= kBatteryIntervalMs)
    {
        last_battery_read       = now_ms;
        const bool was_present  = battery_present;
        const int  was_level    = battery_level;
        const bool was_charging = battery_charging;
        readBattery();
        if (battery_present != was_present || battery_level != was_level || battery_charging != was_charging)
            force_redraw = true;
    }

    tm now{};
    if (display_ok && screen_on && getLocalTime(&now, 10))
    {
        const int  animation_mode = getAnimationMode();
        const bool animation_due  = animation_mode != 0 && now_ms - last_animation_draw >= kAnimationIntervalMs;
        const bool content_due    = force_redraw
                                    || environment_dirty
                                    || now.tm_sec != last_drawn_second
                                    || animation_mode != last_animation_mode;
        if (content_due || animation_due)
        {
            if (animation_due)
            {
                last_animation_draw = now_ms;
                ++animation_frame;
            }
            drawScreen(now, animation_mode);
        }
    }

    delay(10);
}
