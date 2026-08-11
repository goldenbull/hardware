#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "lunar_calendar.h"
#include "runtime_config.h"

namespace
{
RuntimeConfig runtime_config = defaultConfig();


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
constexpr uint32_t kClockRetryMs          = 10000;
// How long one NTP request is given before it counts as failed; the rest of
// kClockRetryMs is the pause before the next request goes out.
constexpr uint32_t kNtpAttemptMs          = 5000;
constexpr uint32_t kBrightnessOverlayMs   = 1500;
constexpr uint32_t kButtonRepeatDelayMs   = 500;
constexpr uint32_t kButtonRepeatRateMs    = 150;
// Button B and C held together for this long opens the manual clock setup.
constexpr uint32_t kSetupHoldMs  = 3000;
constexpr uint32_t kSetupBlinkMs = 400;
// The lower bound is where the epoch starts; the upper is where the lunar table
// runs out, and there is no reason to hand-set a clock beyond either.
constexpr int kSetupMinYear = 1970;
constexpr int kSetupMaxYear = 2100;

// Order in which Button A walks the fields. One full pass ends the setup.
enum SetupField : uint8_t
{
    kFieldYear,
    kFieldMonth,
    kFieldDay,
    kFieldHour,
    kFieldMinute,
    kFieldCount,
};

// Backlight duty ladder, spaced roughly geometrically so the steps look even.
// The first entry is the dimmest setting and is deliberately non-zero: the
// screen stays readable in the dark, only Button A turns the backlight off.
constexpr uint8_t kBrightnessLevels[] = {16, 40, 80, 150, 255};
constexpr int     kBrightnessCount    = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);
constexpr int     kBrightnessDefault  = 2; // the third step, duty 80

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

// Which stage of the startup sync the corner label reports. Written by the sync
// task, read by the draw loop, so it stays a single volatile word.
enum ClockSyncStage : uint8_t
{
    kStageWifi,   // still associating with the access point
    kStageNtp,    // link is up, waiting for the NTP reply
    kStageFailed, // the last request went unanswered; the next one is pending
};
volatile ClockSyncStage clock_sync_stage = kStageWifi;
// Set from the SNTP task. A wall-clock test would not do as a stand-in: setting
// the date by hand also moves the clock into the present.
volatile bool ntp_synced = false;

int      brightness_index         = kBrightnessDefault;
bool     brightness_overlay_shown = false;
uint32_t brightness_overlay_until = 0;
// Shared by the brightness steps and the manual-setup steps; the two modes are
// mutually exclusive, so one pair of repeat timers covers both.
uint32_t btn_b_repeat_at = 0;
uint32_t btn_c_repeat_at = 0;

bool     setup_mode          = false;
int      setup_field         = kFieldYear;
tm       setup_time{};
uint32_t setup_hold_since    = 0;
uint32_t setup_blink_at      = 0;
bool     setup_blink_on      = true;
bool     setup_await_release = false;

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

int daysInMonth(int year, int month)
{
    static constexpr int kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return kLengths[month - 1];
}

// Refills tm_wday and tm_yday so the weekday and the lunar date stay correct
// while the user scrolls through the fields.
void normalizeSetupTime()
{
    setup_time.tm_sec   = 0;
    setup_time.tm_isdst = -1;
    tm normalized       = setup_time;
    if (mktime(&normalized) != static_cast<time_t>(-1))
        setup_time = normalized;
}

void adjustSetupField(int delta)
{
    switch (setup_field)
    {
    case kFieldYear:
    {
        // Clamped rather than wrapped: rolling over a 130-year span would make
        // the year unusable to step through.
        const int year = setup_time.tm_year + 1900 + delta;
        if (year >= kSetupMinYear && year <= kSetupMaxYear)
            setup_time.tm_year = year - 1900;
        break;
    }
    case kFieldMonth:
        setup_time.tm_mon = (setup_time.tm_mon + delta + 12) % 12;
        break;
    case kFieldDay:
    {
        const int days     = daysInMonth(setup_time.tm_year + 1900, setup_time.tm_mon + 1);
        setup_time.tm_mday = (setup_time.tm_mday - 1 + delta + days) % days + 1;
        break;
    }
    case kFieldHour:
        setup_time.tm_hour = (setup_time.tm_hour + delta + 24) % 24;
        break;
    case kFieldMinute:
        setup_time.tm_min = (setup_time.tm_min + delta + 60) % 60;
        break;
    default:
        break;
    }

    // Stepping the month or the year can strand the day past the month's end.
    const int days = daysInMonth(setup_time.tm_year + 1900, setup_time.tm_mon + 1);
    if (setup_time.tm_mday > days)
        setup_time.tm_mday = days;

    normalizeSetupTime();
    force_redraw = true;
}

void enterSetupMode(const tm& now)
{
    setup_mode  = true;
    setup_field = kFieldYear;
    setup_time  = now;
    if (setup_time.tm_year + 1900 < kSetupMinYear)
        setup_time.tm_year = kSetupMinYear - 1900;
    normalizeSetupTime();

    setup_blink_on = true;
    setup_blink_at = millis() + kSetupBlinkMs;
    // B and C are still down from the entry gesture; ignore them until they are
    // released so the first field does not start racing straight away.
    setup_await_release = true;
    force_redraw        = true;
    Serial.println("Manual clock setup: entered");
}

void exitSetupMode(bool commit)
{
    setup_mode       = false;
    setup_hold_since = 0;
    force_redraw     = true;

    if (!commit)
    {
        Serial.println("Manual clock setup: abandoned, NTP took over");
        return;
    }

    normalizeSetupTime();
    tm           entered = setup_time;
    const time_t epoch   = mktime(&entered);
    if (epoch == static_cast<time_t>(-1))
    {
        Serial.println("Manual clock setup: the entered date is not representable, discarded");
        return;
    }

    const timeval value{epoch, 0};
    settimeofday(&value, nullptr);
    Serial.printf("Manual clock setup: clock set to %04d-%02d-%02d %02d:%02d:00\n",
                  setup_time.tm_year + 1900,
                  setup_time.tm_mon + 1,
                  setup_time.tm_mday,
                  setup_time.tm_hour,
                  setup_time.tm_min);
}

// True once B and C have been held together long enough to open the setup.
bool setupGestureHeld(uint32_t now_ms)
{
    if (!M5.BtnB.isPressed() || !M5.BtnC.isPressed())
    {
        setup_hold_since = 0;
        return false;
    }
    if (setup_hold_since == 0)
    {
        setup_hold_since = now_ms;
        return false;
    }
    return now_ms - setup_hold_since >= kSetupHoldMs;
}

// Runs on the SNTP task once a reply has been applied to the system clock.
void onNtpSync(timeval*)
{
    ntp_synced = true;
}

bool clockIsSynced()
{
    return ntp_synced;
}

// Sleeps up to duration_ms, cut short as soon as the clock lands or the link
// drops, so the corner label never trails the real state by more than a poll.
// Returns whether the clock is now synced.
bool waitForClock(uint32_t duration_ms)
{
    const uint32_t deadline = millis() + duration_ms;
    while (static_cast<int32_t>(millis() - deadline) < 0)
    {
        if (clockIsSynced())
            return true;
        if (WiFi.status() != WL_CONNECTED)
            return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return clockIsSynced();
}

// Wi-Fi association and the NTP round trip both block for seconds at a time, so
// they run here instead of in setup(): the clock draws epoch time from the first
// frame and silently corrects itself once the sync completes.
void clockSyncTask(void*)
{
    if (runtime_config.wifi_ssid.isEmpty())
    {
        Serial.println("Wi-Fi is not configured, the clock stays on epoch time");
        clock_sync_stage = kStageFailed;
        vTaskDelete(nullptr);
        return;
    }

    Serial.printf("Connecting to Wi-Fi SSID: %s\n", runtime_config.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(runtime_config.wifi_ssid.c_str(), runtime_config.wifi_password.c_str());

    bool announced = false;
    while (!clockIsSynced())
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            clock_sync_stage = kStageWifi;
            announced        = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!announced)
        {
            Serial.print("Wi-Fi connected, IP: ");
            Serial.println(WiFi.localIP());
            announced = true;
        }

        // Re-armed on every pass: a request that went out before the link was
        // usable would otherwise not be retried until the sync interval elapsed.
        clock_sync_stage = kStageNtp;
        esp_sntp_set_sync_interval(kNtpSyncIntervalMs);
        configTzTime(runtime_config.timezone.c_str(), runtime_config.ntp_server.c_str());
        if (waitForClock(kNtpAttemptMs))
            break;

        // This attempt went unanswered, so say so until the next one goes out.
        clock_sync_stage = kStageFailed;
        waitForClock(kClockRetryMs - kNtpAttemptMs);
    }

    Serial.println("Clock synchronised from NTP");
    vTaskDelete(nullptr);
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
    if (temperature < runtime_config.cold_threshold)
        return -1;
    if (temperature > runtime_config.hot_threshold)
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
    constexpr int kSegmentWidth  = 11;
    constexpr int kSegmentHeight = 8;
    constexpr int kSegmentPitch  = 14;

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

// Largest text size no wider than max_width. The caller selects the font first.
float fitTextSize(const char* text, int max_width, float max_size)
{
    for (float size = max_size; size > 1.0f; size -= 0.05f)
    {
        canvas.setTextSize(size);
        if (canvas.textWidth(text) <= max_width)
            return size;
    }
    canvas.setTextSize(1.0f);
    return 1.0f;
}

// Bare readings, no labels: the colour tells temperature and humidity apart.
void drawEnvironment()
{
    constexpr int   kTop       = 184;
    constexpr int   kGap       = 24;  // between the two readings
    constexpr int   kUnitGap   = 3;   // between a reading and its unit
    constexpr float kValueSize = 0.65f;

    canvas.setTextSize(1.0f);
    canvas.setTextDatum(top_left);

    if (!sensor_ok)
    {
        canvas.setFont(&fonts::efontCN_24);
        canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
        canvas.drawCenterString("温湿度传感器不可用", 160, kTop + 12);
        return;
    }

    char temp[8];
    char rh[8];
    snprintf(temp, sizeof(temp), "%.1f", temperature);
    snprintf(rh, sizeof(rh), "%.0f", humidity);

    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(kValueSize);
    const int value_height = canvas.fontHeight();
    const int temp_width   = canvas.textWidth(temp);
    const int rh_width     = canvas.textWidth(rh);

    // Font7 has no degree or percent glyph, so the units come from the CJK font,
    // scaled to the digit height instead of left at its own.
    canvas.setFont(&fonts::efontCN_24);
    canvas.setTextSize(1.0f);
    const float unit_size = static_cast<float>(value_height) / canvas.fontHeight();
    canvas.setTextSize(unit_size);
    const int temp_unit_width = canvas.textWidth("°C");
    const int rh_unit_width   = canvas.textWidth("%");

    const int total = temp_width + kUnitGap + temp_unit_width + kGap + rh_width + kUnitGap + rh_unit_width;
    int       x     = 160 - total / 2;

    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(kValueSize);
    canvas.drawString(temp, x, kTop);
    x += temp_width + kUnitGap;
    canvas.setFont(&fonts::efontCN_24);
    canvas.setTextSize(unit_size);
    canvas.drawString("°C", x, kTop);
    x += temp_unit_width + kGap;

    canvas.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(kValueSize);
    canvas.drawString(rh, x, kTop);
    x += rh_width + kUnitGap;
    canvas.setFont(&fonts::efontCN_24);
    canvas.setTextSize(unit_size);
    canvas.drawString("%", x, kTop);
    canvas.setTextSize(1.0f);
}

bool fieldHidden(int field)
{
    return setup_mode && !setup_blink_on && setup_field == field;
}

// Paints over one substring of a string already drawn at `left`, which is how
// the field under edit blinks. Cheaper than re-rendering the row in pieces, and
// exact for any font: the caller only has to leave the font and size in place.
void blankTextSpan(const char* text, int offset, int length, int left, int top)
{
    char buffer[16];
    if (offset >= static_cast<int>(sizeof(buffer)) || length >= static_cast<int>(sizeof(buffer)))
        return;

    memcpy(buffer, text, offset);
    buffer[offset] = '\0';
    const int x    = left + canvas.textWidth(buffer);

    memcpy(buffer, text + offset, length);
    buffer[length] = '\0';
    canvas.fillRect(x, top, canvas.textWidth(buffer), canvas.fontHeight(), TFT_BLACK);
}

// Icons above the three front buttons, shown only during manual setup: a ring
// with an arrowhead for "next field", then minus and plus.
void drawButtonHints()
{
    constexpr int   kCenterY   = 220;
    constexpr int   kRadius    = 11;
    constexpr int   kCenters[] = {64, 160, 256};
    constexpr float kArcEnd    = 300.0f; // degrees; the gap runs from here to 20

    canvas.drawArc(kCenters[0], kCenterY, kRadius - 3, kRadius, 20.0f, kArcEnd, TFT_WHITE);

    // Arrowhead closing the ring: tip along the tangent, base across the radius,
    // so the gap reads as rotation rather than as a broken circle.
    const float cos_end = cosf(kArcEnd * DEG_TO_RAD);
    const float sin_end = sinf(kArcEnd * DEG_TO_RAD);
    const int   end_x   = kCenters[0] + static_cast<int>(cos_end * (kRadius - 1.5f));
    const int   end_y   = kCenterY + static_cast<int>(sin_end * (kRadius - 1.5f));
    canvas.fillTriangle(end_x - static_cast<int>(sin_end * 8),
                        end_y + static_cast<int>(cos_end * 8),
                        end_x + static_cast<int>(cos_end * 5),
                        end_y + static_cast<int>(sin_end * 5),
                        end_x - static_cast<int>(cos_end * 5),
                        end_y - static_cast<int>(sin_end * 5),
                        TFT_WHITE);

    canvas.fillRect(kCenters[1] - 10, kCenterY - 2, 20, 4, TFT_WHITE);
    canvas.fillRect(kCenters[2] - 10, kCenterY - 2, 20, 4, TFT_WHITE);
    canvas.fillRect(kCenters[2] - 2, kCenterY - 10, 4, 20, TFT_WHITE);
}

// The weekday is drawn separately and a size down: full-width CJK glyphs look a
// good deal heavier than the half-width digits at any shared text size.
void drawGregorianDate(const tm& now, int top)
{
    static const char* const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    constexpr int            kGap       = 14;
    constexpr float          kWeekSize  = 1.0f;

    char date[16];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    const char* weekday = weekdays[now.tm_wday];

    const float date_size = fitTextSize(date, 200, 1.5f);
    const int   date_width  = canvas.textWidth(date);
    const int   date_height = canvas.fontHeight();

    canvas.setTextSize(kWeekSize);
    const int week_width  = canvas.textWidth(weekday);
    const int week_height = canvas.fontHeight();

    int x = 160 - (date_width + kGap + week_width) / 2;
    canvas.setTextSize(date_size);
    canvas.drawString(date, x, top);
    // Offsets into "%04d-%02d-%02d".
    if (fieldHidden(kFieldYear))
        blankTextSpan(date, 0, 4, x, top);
    else if (fieldHidden(kFieldMonth))
        blankTextSpan(date, 5, 2, x, top);
    else if (fieldHidden(kFieldDay))
        blankTextSpan(date, 8, 2, x, top);

    canvas.setTextSize(kWeekSize);
    canvas.drawString(weekday, x + date_width + kGap, top + (date_height - week_height) / 2);
    canvas.setTextSize(1.0f);
}

// The date row alternates between the two calendars. Both variants are centred
// inside the same fixed-height band so the row does not jump on the swap.
void drawDate(const tm& now)
{
    constexpr int kTop       = 34;
    constexpr int kRowHeight = 36; // efontCN_24 at 1.5x, the taller variant
    constexpr int kSwapSecs  = 2;

    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setFont(&fonts::efontCN_24);

    // The setup mode edits Gregorian fields and freezes the seconds, so the
    // alternation is pinned off while it is open.
    char lunar[48];
    if (!setup_mode && (now.tm_sec / kSwapSecs) % 2 != 0 && formatLunarDate(now, lunar, sizeof(lunar)))
    {
        // All full-width glyphs here, so this stays a size below the Gregorian
        // digits to keep the two frames at a similar visual weight.
        canvas.setTextSize(fitTextSize(lunar, 312, 1.25f));
        canvas.drawCenterString(lunar, 160, kTop + (kRowHeight - canvas.fontHeight()) / 2);
        canvas.setTextSize(1.0f);
        return;
    }

    drawGregorianDate(now, kTop);
}

void handleNormalButtons(uint32_t now_ms)
{
    if (M5.BtnA.wasPressed())
    {
        screen_on = !screen_on;
        applyBrightness();
        if (screen_on)
            force_redraw = true;
    }

    // Button B dims, Button C brightens. Both are ignored while the backlight is
    // off so that Button A stays the only way back from a dark screen, and while
    // both are down, because that is the manual-setup gesture rather than a
    // brightness change.
    const bool both_held = M5.BtnB.isPressed() && M5.BtnC.isPressed();
    if (screen_on && !both_held)
    {
        const bool dim      = buttonRepeat(M5.BtnB, btn_b_repeat_at, now_ms);
        const bool brighten = buttonRepeat(M5.BtnC, btn_c_repeat_at, now_ms);
        if (dim || brighten)
        {
            adjustBrightness(dim ? -1 : 1);
            brightness_overlay_shown = true;
            brightness_overlay_until = now_ms + kBrightnessOverlayMs;
            force_redraw             = true;
        }
    }
}

// A steps to the next field and ends the setup after the last one; B and C step
// the current field down and up.
void handleSetupButtons(uint32_t now_ms)
{
    if (M5.BtnA.wasPressed())
    {
        if (++setup_field >= kFieldCount)
        {
            exitSetupMode(true);
            return;
        }
        setup_blink_on = true;
        setup_blink_at = now_ms + kSetupBlinkMs;
        force_redraw   = true;
    }

    if (setup_await_release)
    {
        if (!M5.BtnB.isPressed() && !M5.BtnC.isPressed())
            setup_await_release = false;
    }
    else
    {
        const bool minus = buttonRepeat(M5.BtnB, btn_b_repeat_at, now_ms);
        const bool plus  = buttonRepeat(M5.BtnC, btn_c_repeat_at, now_ms);
        if (minus != plus)
            adjustSetupField(minus ? -1 : 1);
    }

    if (static_cast<int32_t>(now_ms - setup_blink_at) >= 0)
    {
        setup_blink_on = !setup_blink_on;
        setup_blink_at = now_ms + kSetupBlinkMs;
        force_redraw   = true;
    }
}

void drawScreen(const tm& now, int animation_mode)
{
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);

    canvas.fillScreen(TFT_BLACK);
    if (animation_mode < 0)
        drawSnow(animation_frame);
    else if (animation_mode > 0)
        drawFire(animation_frame);
    drawBattery();
    if (brightness_overlay_shown)
        drawBrightnessOverlay();
    else if (!clockIsSynced())
    {
        const bool  failed = clock_sync_stage == kStageFailed;
        const char* label  = failed                          ? "校时失败"
                             : clock_sync_stage == kStageWifi ? "WiFi连接中"
                                                             : "校时中";

        // Same corner as the brightness bar, so it yields while that is up.
        canvas.setTextDatum(top_left);
        canvas.setTextColor(failed ? TFT_RED : TFT_ORANGE, TFT_BLACK);
        canvas.setFont(&fonts::efontCN_24); // scaled rather than efontCN_16: a
        canvas.setTextSize(0.7f);           // second CJK font costs 300+ KB of flash
        canvas.drawString(label, 6, 6);
        canvas.setTextSize(1.0f);
    }

    drawDate(now);

    constexpr int kClockTop = 96;
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(fitTextSize(clock, 312, 1.15f));
    const int clock_left = 160 - canvas.textWidth(clock) / 2;
    canvas.drawCenterString(clock, 160, kClockTop);
    // Offsets into "%02d:%02d:%02d".
    if (fieldHidden(kFieldHour))
        blankTextSpan(clock, 0, 2, clock_left, kClockTop);
    else if (fieldHidden(kFieldMinute))
        blankTextSpan(clock, 3, 2, clock_left, kClockTop);
    canvas.setTextSize(1.0f);

    // The hints sit where the readings would, so only one of the two is up.
    if (setup_mode)
        drawButtonHints();
    else
        drawEnvironment();

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

    auto m5_config          = M5.config();
    m5_config.clear_display = true;
    M5.begin(m5_config);
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

    // The TF card shares the LCD's SPI bus, so this has to come after M5.begin().
    loadConfigFromSd(runtime_config);

    // Set before anything can render, so pre-sync epoch time is still shown in
    // the configured zone rather than UTC.
    setenv("TZ", runtime_config.timezone.c_str(), 1);
    tzset();

    Wire.begin(21, 22);
    sensor_present    = detectSht30();
    last_sensor_probe = millis();
    Serial.printf("SHT30 sensor: %s\n", sensor_present ? "detected" : "not detected");

    readBattery();
    last_battery_read = millis();
    Serial.printf("IP5306 power IC: %s\n", battery_present ? "detected" : "not detected");

    // Registered before SNTP starts; configTzTime() does not touch the callback.
    sntp_set_time_sync_notification_cb(onNtpSync);
    xTaskCreate(clockSyncTask, "clock_sync", 4096, nullptr, 1, nullptr);
}

void loop()
{
    M5.update();
    const uint32_t now_ms = millis();

    // NTP is authoritative: if it lands mid-edit the half-entered value is
    // dropped, and the gesture cannot reopen the setup afterwards.
    if (setup_mode && clockIsSynced())
        exitSetupMode(false);

    if (setup_mode)
    {
        handleSetupButtons(now_ms);
    }
    else
    {
        handleNormalButtons(now_ms);
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

    // Deliberately not getLocalTime(): that rejects any year before 2016, which
    // would leave the screen blank until NTP replies.
    const time_t epoch = time(nullptr);
    tm           now{};
    localtime_r(&epoch, &now);

    // Evaluated unconditionally so the hold timer cannot survive across a
    // backlight toggle and fire the moment the screen comes back on.
    const bool gesture_held = setupGestureHeld(now_ms);
    if (!setup_mode && !clockIsSynced() && screen_on && gesture_held)
        enterSetupMode(now);

    // The setup edits a frozen copy, so the display follows that instead of the
    // running clock while it is open.
    const tm& shown = setup_mode ? setup_time : now;
    if (display_ok && screen_on)
    {
        // Requirement of the setup mode: no weather animation behind the fields.
        const int  animation_mode = setup_mode ? 0 : getAnimationMode();
        const bool animation_due  = animation_mode != 0 && now_ms - last_animation_draw >= kAnimationIntervalMs;
        const bool content_due    = force_redraw
                                    || environment_dirty
                                    || shown.tm_sec != last_drawn_second
                                    || animation_mode != last_animation_mode;
        if (content_due || animation_due)
        {
            if (animation_due)
            {
                last_animation_draw = now_ms;
                ++animation_frame;
            }
            drawScreen(shown, animation_mode);
        }
    }

    delay(10);
}
