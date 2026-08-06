#include <M5Unified.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif
#ifndef APP_NTP_SERVER
#define APP_NTP_SERVER "pool.ntp.org"
#endif
#ifndef APP_TIMEZONE
#define APP_TIMEZONE "CST-8"
#endif

namespace {
constexpr uint8_t kSht30Address = 0x44;
constexpr uint32_t kSensorIntervalMs = 2000;
constexpr uint32_t kNtpSyncIntervalMs = 60U * 60U * 1000U;
constexpr uint32_t kAnimationIntervalMs = 50;

bool screen_on = true;
bool sensor_ok = false;
float temperature = NAN;
float humidity = NAN;
uint32_t last_sensor_read = 0;
uint32_t last_frame_draw = 0;
uint32_t animation_frame = 0;

uint8_t crc8(const uint8_t* data, size_t size) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool readSht30(float& temp, float& rh) {
  Wire.beginTransmission(kSht30Address);
  Wire.write(0x2C);  // High repeatability, clock stretching enabled.
  Wire.write(0x06);
  if (Wire.endTransmission() != 0) return false;
  delay(20);

  if (Wire.requestFrom(kSht30Address, static_cast<uint8_t>(6)) != 6) return false;
  uint8_t data[6];
  for (auto& byte : data) byte = Wire.read();
  if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) return false;

  const uint16_t raw_temp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
  const uint16_t raw_rh = (static_cast<uint16_t>(data[3]) << 8) | data[4];
  temp = -45.0f + 175.0f * raw_temp / 65535.0f;
  rh = 100.0f * raw_rh / 65535.0f;
  return true;
}

void connectWifi() {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::efontCN_24);
  M5.Display.drawString("正在连接 Wi-Fi...", 160, 100);
  if (APP_WIFI_SSID[0] == '\0') {
    M5.Display.drawString("未设置 WIFI_SSID", 160, 140);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
  }
}

void startClockSync() {
  setenv("TZ", APP_TIMEZONE, 1);
  tzset();
  esp_sntp_set_sync_interval(kNtpSyncIntervalMs);
  configTzTime(APP_TIMEZONE, APP_NTP_SERVER);

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::efontCN_24);
  M5.Display.drawCenterString("正在校准时间...", 160, 105);
  tm now{};
  for (int attempt = 0; attempt < 30 && !getLocalTime(&now, 1000); ++attempt) {
    delay(10);
  }
}

void drawSnow(uint32_t frame) {
  // Each flake has a stable horizontal position and a slightly different speed.
  for (int i = 0; i < 14; ++i) {
    const int x = (i * 47 + 19) % 320;
    const int speed = 1 + i % 3;
    const int y = (static_cast<int>(frame) * speed + i * 17) % 48;
    const int radius = 1 + i % 2;
    M5.Display.drawLine(x - radius, y, x + radius, y, TFT_WHITE);
    M5.Display.drawLine(x, y - radius, x, y + radius, TFT_WHITE);
    if (radius == 2) {
      M5.Display.drawPixel(x - 1, y - 1, TFT_CYAN);
      M5.Display.drawPixel(x + 1, y + 1, TFT_CYAN);
    }
  }
}

void drawFire(uint32_t frame) {
  constexpr int kBottom = 239;
  constexpr int kFlameCount = 13;
  for (int i = 0; i < kFlameCount; ++i) {
    const int center_x = i * 26 - 4;
    const float wave = sinf(frame * 0.28f + i * 1.37f);
    const int outer_height = 20 + (i * 7) % 14 + static_cast<int>(wave * 5);
    const int inner_height = outer_height * 2 / 3;

    M5.Display.fillTriangle(center_x - 16, kBottom, center_x + 16, kBottom,
                            center_x + static_cast<int>(wave * 5),
                            kBottom - outer_height, TFT_RED);
    M5.Display.fillTriangle(center_x - 10, kBottom, center_x + 10, kBottom,
                            center_x - static_cast<int>(wave * 3),
                            kBottom - inner_height, TFT_ORANGE);
    M5.Display.fillCircle(center_x, kBottom - 4, 6, TFT_YELLOW);
  }
}

void drawScreen(const tm& now) {
  static const char* const weekdays[] = {
      "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
  char date[32];
  char clock[16];
  snprintf(date, sizeof(date), "%04d-%02d-%02d  %s", now.tm_year + 1900,
           now.tm_mon + 1, now.tm_mday, weekdays[now.tm_wday]);
  snprintf(clock, sizeof(clock), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);

  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  if (sensor_ok && temperature < 20.0f) drawSnow(animation_frame);
  if (sensor_ok && temperature > 30.0f) drawFire(animation_frame);

  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setFont(&fonts::efontCN_24);
  M5.Display.drawCenterString(date, 160, 48);

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setFont(&fonts::Font7);
  M5.Display.drawCenterString(clock, 160, 86);

  M5.Display.setTextColor(sensor_ok ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  M5.Display.setFont(&fonts::efontCN_24);
  char environment[64];
  if (sensor_ok) {
    snprintf(environment, sizeof(environment), "温度 %.1f°C    湿度 %.1f%%", temperature, humidity);
  } else {
    snprintf(environment, sizeof(environment), "温湿度传感器不可用");
  }
  M5.Display.drawCenterString(environment, 160, 180);
  M5.Display.endWrite();
}
}  // namespace

extern "C" void app_main() {
  initArduino();
  auto config = M5.config();
  config.clear_display = true;
  M5.begin(config);
  Wire.begin(21, 22);

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) startClockSync();

  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed()) {
      screen_on = !screen_on;
      M5.Display.setBrightness(screen_on ? 128 : 0);
      if (screen_on) last_frame_draw = 0;
    }

    const uint32_t now_ms = millis();
    if (now_ms - last_sensor_read >= kSensorIntervalMs || last_sensor_read == 0) {
      last_sensor_read = now_ms;
      sensor_ok = readSht30(temperature, humidity);
    }

    tm now{};
    if (screen_on && now_ms - last_frame_draw >= kAnimationIntervalMs &&
        getLocalTime(&now, 10)) {
      last_frame_draw = now_ms;
      ++animation_frame;
      drawScreen(now);
    }
    delay(10);
  }
}
