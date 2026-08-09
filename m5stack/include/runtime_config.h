#pragma once

#include <Arduino.h>

// Settings the firmware reads from /clock-config.ini on the TF card at boot.
// Every field starts at its compiled-in default, so a missing card, a missing
// file or a missing key all degrade to the value baked into app_config.h.
struct RuntimeConfig
{
    String wifi_ssid;
    String wifi_password;
    String ntp_server;
    String timezone;
    float  cold_threshold; // below this the snow animation runs
    float  hot_threshold;  // above this the fire animation runs
};

RuntimeConfig defaultConfig();

// Overlays /clock-config.ini from the TF card root onto `config`, leaving
// untouched anything the file does not mention. Returns true when the file was
// found and read. Mounts and unmounts the card around the read: the slot shares
// the SPI bus with the LCD and nothing else needs it after boot.
bool loadConfigFromSd(RuntimeConfig& config);
