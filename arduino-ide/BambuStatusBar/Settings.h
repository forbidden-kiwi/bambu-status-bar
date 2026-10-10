#pragma once

#include <Arduino.h>

#include "Config.h"
#include "LedEffects.h"

// Color + animation for one user-facing LED status.
struct LightSlot {
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  LedEffect effect = LedEffect::Pulse;
};

// Everything the web UI can change, persisted in NVS. The setup pages own
// credentials and printer identity — no secrets in source.
struct DeviceSettings {
  String wifiSsid;
  String wifiPass;
  String hostname = kDefaultHostname;

  String printerIp;
  String printerSerial;
  String accessCode;

  String apSsid = kDefaultApSsid;
  String apPass = kDefaultApPass;
  String apIp = kDefaultApIp;
  bool apEnabled = true;

  uint8_t brightness = led_cfg::kDefaultBrightnessPercent;  // 0..100
  uint8_t speed = led_cfg::kDefaultSpeedPercent;            // 1..100, 50 is 1x

  // Hardware for the WS2812 bar. Changing pin/count requires a reboot.
  uint8_t ledPin = hw::kDefaultDataPin;
  uint16_t ledCount = hw::kDefaultNumLeds;

  // Defaults match the previous hard-coded LED look for each phase.
  LightSlot idle{255, 255, 255, LedEffect::Pulse};
  LightSlot printing{255, 255, 255, LedEffect::Progress};
  LightSlot error{255, 0, 0, LedEffect::Strobe};
  LightSlot homing{255, 120, 0, LedEffect::Comet};
  LightSlot leveling{255, 120, 0, LedEffect::Comet};
  LightSlot preparing{255, 120, 0, LedEffect::Comet};
  LightSlot heating{255, 80, 0, LedEffect::Progress};
  LightSlot downloading{255, 220, 0, LedEffect::Comet};
  LightSlot paused{255, 255, 255, LedEffect::Pulse};
  LightSlot finished{0, 255, 0, LedEffect::Solid};

  bool ledReverse = false;
  // When true, the status bar goes dark while the printer chamber_light is off.
  bool followChamberLight = false;

  String language = "de";
};

class SettingsStore {
 public:
  void begin();
  void save();
  void clear();
  void resetLights();

  DeviceSettings& data() { return data_; }
  const DeviceSettings& data() const { return data_; }
  uint32_t revision() const { return revision_; }

  bool printerReady() const;

 private:
  void clamp();

  DeviceSettings data_;
  uint32_t revision_ = 0;
};
