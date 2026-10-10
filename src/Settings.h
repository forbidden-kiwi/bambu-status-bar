#pragma once

#include <Arduino.h>

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
  String hostname = "BambuStatusBar";

  String printerIp;
  String printerSerial;
  String accessCode;

  String apSsid = "BambuStatusBar";
  String apPass = "987654321";
  String apIp = "192.168.254.1";
  bool apEnabled = true;

  uint8_t brightness = 50;  // 0..100, H2D default is 50%
  uint8_t speed = 50;       // 1..100, 50 is 1x

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
