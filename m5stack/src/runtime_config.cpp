#include "runtime_config.h"

#include <SD.h>
#include <SPI.h>

#include <cstdlib>

#include "app_config.h"

namespace
{
// TF slot on the Core Basic. It shares the VSPI bus with the LCD, which is why
// the card is mounted only for the duration of the read.
constexpr int      kSdCsPin      = 4;
constexpr int      kSdSckPin     = 18;
constexpr int      kSdMisoPin    = 19;
constexpr int      kSdMosiPin    = 23;
constexpr uint32_t kSdFrequency  = 20000000;
constexpr char     kConfigPath[] = "/clock-config.ini";

String trimmed(const String& text)
{
    String out = text;
    out.trim();
    return out;
}

// Strips one matching pair of quotes, which is the only way to give a value
// leading or trailing spaces -- Wi-Fi passwords are allowed to have them.
String unquoted(const String& value)
{
    if (value.length() >= 2)
    {
        const char first = value.charAt(0);
        if ((first == '"' || first == '\'') && value.charAt(value.length() - 1) == first)
            return value.substring(1, value.length() - 1);
    }
    return value;
}

bool parseFloat(const String& value, float& out)
{
    char*       end    = nullptr;
    const float parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0')
        return false;
    out = parsed;
    return true;
}

void applyEntry(RuntimeConfig& config, int line_number, const String& key, const String& value)
{
    if (key.equalsIgnoreCase("wifi_ssid"))
        config.wifi_ssid = value;
    else if (key.equalsIgnoreCase("wifi_password"))
        config.wifi_password = value;
    else if (key.equalsIgnoreCase("ntp_server"))
        config.ntp_server = value;
    else if (key.equalsIgnoreCase("timezone"))
        config.timezone = value;
    else if (key.equalsIgnoreCase("cold_threshold"))
    {
        if (!parseFloat(value, config.cold_threshold))
            Serial.printf("%s:%d cold_threshold is not a number: %s\n", kConfigPath, line_number, value.c_str());
    }
    else if (key.equalsIgnoreCase("hot_threshold"))
    {
        if (!parseFloat(value, config.hot_threshold))
            Serial.printf("%s:%d hot_threshold is not a number: %s\n", kConfigPath, line_number, value.c_str());
    }
    else
    {
        Serial.printf("%s:%d unknown key: %s\n", kConfigPath, line_number, key.c_str());
    }
}
} // namespace

RuntimeConfig defaultConfig()
{
    RuntimeConfig config;
    config.wifi_ssid      = APP_WIFI_SSID;
    config.wifi_password  = APP_WIFI_PASSWORD;
    config.ntp_server     = APP_NTP_SERVER;
    config.timezone       = APP_TIMEZONE;
    config.cold_threshold = APP_COLD_THRESHOLD;
    config.hot_threshold  = APP_HOT_THRESHOLD;
    return config;
}

bool loadConfigFromSd(RuntimeConfig& config)
{
    SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    if (!SD.begin(kSdCsPin, SPI, kSdFrequency))
    {
        Serial.println("No TF card, keeping the compiled-in configuration");
        return false;
    }

    File file = SD.open(kConfigPath, FILE_READ);
    if (!file)
    {
        Serial.printf("%s not found, keeping the compiled-in configuration\n", kConfigPath);
        SD.end();
        return false;
    }

    int line_number = 0;
    while (file.available())
    {
        ++line_number;
        const String line = trimmed(file.readStringUntil('\n'));
        // Section headers are accepted and ignored: the key space is flat, so
        // grouping is purely cosmetic.
        if (line.isEmpty() || line.startsWith("#") || line.startsWith(";") || line.startsWith("["))
            continue;

        const int separator = line.indexOf('=');
        if (separator < 0)
        {
            Serial.printf("%s:%d ignored, no '=': %s\n", kConfigPath, line_number, line.c_str());
            continue;
        }
        applyEntry(config,
                   line_number,
                   trimmed(line.substring(0, separator)),
                   unquoted(trimmed(line.substring(separator + 1))));
    }

    file.close();
    SD.end();

    // An inverted pair would make getAnimationMode() pick fire for every
    // temperature that is also cold enough for snow.
    if (config.cold_threshold >= config.hot_threshold)
    {
        Serial.printf("%s: cold_threshold %.1f is not below hot_threshold %.1f, reverting both to defaults\n",
                      kConfigPath,
                      config.cold_threshold,
                      config.hot_threshold);
        config.cold_threshold = APP_COLD_THRESHOLD;
        config.hot_threshold  = APP_HOT_THRESHOLD;
    }

    Serial.printf("Loaded %s: ssid=%s ntp=%s tz=%s cold=%.1f hot=%.1f\n",
                  kConfigPath,
                  config.wifi_ssid.c_str(),
                  config.ntp_server.c_str(),
                  config.timezone.c_str(),
                  config.cold_threshold,
                  config.hot_threshold);
    return true;
}
