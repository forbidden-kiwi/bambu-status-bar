#include "Settings.h"

#include <Preferences.h>

namespace {
constexpr const char* kNs = "bambu";

void loadSlot(Preferences& prefs, LightSlot& slot, const char* rk, const char* gk, const char* bk,
              const char* fxk, uint8_t defR, uint8_t defG, uint8_t defB, LedEffect defFx) {
  slot.r = prefs.getUChar(rk, defR);
  slot.g = prefs.getUChar(gk, defG);
  slot.b = prefs.getUChar(bk, defB);
  slot.effect = clampLedEffect(prefs.getUChar(fxk, static_cast<uint8_t>(defFx)), defFx);
}

void saveSlot(Preferences& prefs, const LightSlot& slot, const char* rk, const char* gk, const char* bk,
              const char* fxk) {
  prefs.putUChar(rk, slot.r);
  prefs.putUChar(gk, slot.g);
  prefs.putUChar(bk, slot.b);
  prefs.putUChar(fxk, static_cast<uint8_t>(slot.effect));
}

void clampSlot(LightSlot& slot, LedEffect fallback) {
  slot.effect = clampLedEffect(static_cast<uint8_t>(slot.effect), fallback);
}
}  // namespace

void SettingsStore::begin() {
  Preferences prefs;
  prefs.begin(kNs, /*readOnly=*/true);
  data_.wifiSsid = prefs.getString("wssid", "");
  data_.wifiPass = prefs.getString("wpass", "");
  data_.hostname = prefs.getString("host", kDefaultHostname);
  data_.printerIp = prefs.getString("ip", "");
  data_.printerSerial = prefs.getString("serial", "");
  data_.accessCode = prefs.getString("code", "");
  data_.apSsid = prefs.getString("apssid", kDefaultApSsid);
  data_.apPass = prefs.getString("appass", kDefaultApPass);
  data_.apIp = prefs.getString("apip", kDefaultApIp);
  data_.apEnabled = prefs.getUChar("apen", 1) != 0;
  data_.brightness = prefs.getUChar("bri", led_cfg::kDefaultBrightnessPercent);
  data_.speed = prefs.getUChar("spd", led_cfg::kDefaultSpeedPercent);
  data_.finishedMinutes = prefs.getUChar("finmin", led_cfg::kDefaultFinishedMinutes);
  data_.ledPin = prefs.getUChar("ledpin", hw::kDefaultDataPin);
  data_.ledCount = prefs.getUShort("ledcnt", hw::kDefaultNumLeds);

  loadSlot(prefs, data_.idle, "ir", "ig", "ib", "idlefx", 255, 255, 255, LedEffect::Pulse);
  loadSlot(prefs, data_.printing, "pr", "pg", "pb", "printfx", 255, 255, 255, LedEffect::Progress);
  loadSlot(prefs, data_.error, "er", "eg", "eb", "errorfx", 255, 0, 0, LedEffect::Strobe);
  loadSlot(prefs, data_.homing, "hr", "hg", "hb", "hfx", 255, 120, 0, LedEffect::Comet);
  loadSlot(prefs, data_.leveling, "lr", "lg", "lb", "lfx", 255, 120, 0, LedEffect::Comet);
  loadSlot(prefs, data_.preparing, "qr", "qg", "qb", "qfx", 255, 120, 0, LedEffect::Comet);
  loadSlot(prefs, data_.heating, "tr", "tg", "tb", "tfx", 255, 80, 0, LedEffect::Progress);
  loadSlot(prefs, data_.downloading, "dr", "dg", "db", "dfx", 255, 220, 0, LedEffect::Comet);
  loadSlot(prefs, data_.paused, "ur", "ug", "ub", "ufx", 255, 255, 255, LedEffect::Pulse);
  loadSlot(prefs, data_.finished, "fr", "fg", "fb", "ffx", 0, 255, 0, LedEffect::Solid);

  data_.ledReverse = prefs.getUChar("ledrev", 0) != 0;
  data_.followChamberLight = prefs.getUChar("follight", 0) != 0;
  data_.language = prefs.getString("lang", "de");
  prefs.end();
  clamp();
  revision_++;
}

void SettingsStore::clamp() {
  if (data_.hostname.isEmpty()) data_.hostname = kDefaultHostname;
  if (data_.apSsid.isEmpty()) data_.apSsid = kDefaultApSsid;
  if (data_.apPass.isEmpty()) data_.apPass = kDefaultApPass;
  if (data_.apIp.isEmpty()) data_.apIp = kDefaultApIp;
  if (data_.brightness > 100) data_.brightness = 100;
  if (data_.speed < 1) data_.speed = 1;
  if (data_.speed > 100) data_.speed = 100;
  if (data_.finishedMinutes < led_cfg::kMinFinishedMinutes) {
    data_.finishedMinutes = led_cfg::kMinFinishedMinutes;
  }
  if (data_.finishedMinutes > led_cfg::kMaxFinishedMinutes) {
    data_.finishedMinutes = led_cfg::kMaxFinishedMinutes;
  }
  if (!hw::isSupportedLedDataPin(data_.ledPin)) data_.ledPin = hw::kDefaultDataPin;
  if (data_.ledCount < 1) data_.ledCount = 1;
  if (data_.ledCount > hw::kMaxNumLeds) data_.ledCount = hw::kMaxNumLeds;
  clampSlot(data_.idle, LedEffect::Pulse);
  clampSlot(data_.printing, LedEffect::Progress);
  clampSlot(data_.error, LedEffect::Strobe);
  clampSlot(data_.homing, LedEffect::Comet);
  clampSlot(data_.leveling, LedEffect::Comet);
  clampSlot(data_.preparing, LedEffect::Comet);
  clampSlot(data_.heating, LedEffect::Progress);
  clampSlot(data_.downloading, LedEffect::Comet);
  clampSlot(data_.paused, LedEffect::Pulse);
  clampSlot(data_.finished, LedEffect::Solid);
  if (data_.language != "de" && data_.language != "en") data_.language = "de";
}

void SettingsStore::save() {
  clamp();
  Preferences prefs;
  prefs.begin(kNs, /*readOnly=*/false);
  prefs.putString("wssid", data_.wifiSsid);
  prefs.putString("wpass", data_.wifiPass);
  prefs.putString("host", data_.hostname);
  prefs.putString("ip", data_.printerIp);
  prefs.putString("serial", data_.printerSerial);
  prefs.putString("code", data_.accessCode);
  prefs.putString("apssid", data_.apSsid);
  prefs.putString("appass", data_.apPass);
  prefs.putString("apip", data_.apIp);
  prefs.putUChar("apen", data_.apEnabled ? 1 : 0);
  prefs.putUChar("bri", data_.brightness);
  prefs.putUChar("spd", data_.speed);
  prefs.putUChar("finmin", data_.finishedMinutes);
  prefs.putUChar("ledpin", data_.ledPin);
  prefs.putUShort("ledcnt", data_.ledCount);
  saveSlot(prefs, data_.idle, "ir", "ig", "ib", "idlefx");
  saveSlot(prefs, data_.printing, "pr", "pg", "pb", "printfx");
  saveSlot(prefs, data_.error, "er", "eg", "eb", "errorfx");
  saveSlot(prefs, data_.homing, "hr", "hg", "hb", "hfx");
  saveSlot(prefs, data_.leveling, "lr", "lg", "lb", "lfx");
  saveSlot(prefs, data_.preparing, "qr", "qg", "qb", "qfx");
  saveSlot(prefs, data_.heating, "tr", "tg", "tb", "tfx");
  saveSlot(prefs, data_.downloading, "dr", "dg", "db", "dfx");
  saveSlot(prefs, data_.paused, "ur", "ug", "ub", "ufx");
  saveSlot(prefs, data_.finished, "fr", "fg", "fb", "ffx");
  prefs.putUChar("ledrev", data_.ledReverse ? 1 : 0);
  prefs.putUChar("follight", data_.followChamberLight ? 1 : 0);
  prefs.putString("lang", data_.language);
  prefs.end();
  revision_++;
}

void SettingsStore::clear() {
  Preferences prefs;
  prefs.begin(kNs, /*readOnly=*/false);
  prefs.clear();
  prefs.end();
  data_ = DeviceSettings{};
  revision_++;
}

void SettingsStore::resetLights() {
  data_.brightness = led_cfg::kDefaultBrightnessPercent;
  data_.speed = led_cfg::kDefaultSpeedPercent;
  data_.finishedMinutes = led_cfg::kDefaultFinishedMinutes;
  data_.idle = LightSlot{255, 255, 255, LedEffect::Pulse};
  data_.printing = LightSlot{255, 255, 255, LedEffect::Progress};
  data_.error = LightSlot{255, 0, 0, LedEffect::Strobe};
  data_.homing = LightSlot{255, 120, 0, LedEffect::Comet};
  data_.leveling = LightSlot{255, 120, 0, LedEffect::Comet};
  data_.preparing = LightSlot{255, 120, 0, LedEffect::Comet};
  data_.heating = LightSlot{255, 80, 0, LedEffect::Progress};
  data_.downloading = LightSlot{255, 220, 0, LedEffect::Comet};
  data_.paused = LightSlot{255, 255, 255, LedEffect::Pulse};
  data_.finished = LightSlot{0, 255, 0, LedEffect::Solid};
  data_.ledReverse = false;
  // Keep followChamberLight — it is a behaviour preference, not a color preset.
  save();
}

bool SettingsStore::printerReady() const {
  return data_.printerIp.length() > 0 && data_.printerSerial.length() > 0 && data_.accessCode.length() > 0;
}
