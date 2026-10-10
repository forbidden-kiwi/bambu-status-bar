// GENERATED FILE — do not edit directly.
// Source of truth is firmware/src/*.{h,cpp}; regenerate with:
//   python3 firmware/tools/generate_arduino_sketch.py

#include <sdkconfig.h>
#include <ArduinoJson.h>
#include <cstring>
#include <Arduino.h>
#include <cstdint>
#include <FastLED.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <Preferences.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <cctype>
#include <cstdio>
#include <vector>
#include <esp_ota_ops.h>

// ==== Config.h ====
// Product version shown in the web UI and compared against GitHub Releases.
constexpr const char* kFirmwareVersion = "1.0.0";

// Public GitHub repo used for over-the-air updates (no token on device).
constexpr const char* kGithubOwner = "forbidden-kiwi";
constexpr const char* kGithubRepo = "bambu-status-bar";
constexpr const char* kGithubAssetName = "firmware.bin";

// Board pins come from the PlatformIO environment so the ESP32-C6 Super Mini
// (LED data on GPIO19, BOOT on GPIO9) and a classic ESP32 dev board can share
// this source. GPIO9 is the C6 strapping/BOOT pin: holding it during reset
// enters the ROM download mode, so the factory-reset gesture is a 3s hold
// while the firmware is already running.
#if __has_include(<sdkconfig.h>)
#endif
#if !defined(STATUS_LED_PIN) && defined(CONFIG_IDF_TARGET_ESP32C6)
#define STATUS_LED_PIN 19
#endif
#if !defined(STATUS_BOOT_PIN) && defined(CONFIG_IDF_TARGET_ESP32C6)
#define STATUS_BOOT_PIN 9
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 5
#endif
#ifndef STATUS_BOOT_PIN
#define STATUS_BOOT_PIN 0
#endif

namespace hw {
constexpr int kDataPin = STATUS_LED_PIN;
constexpr int kResetButtonPin = STATUS_BOOT_PIN;
constexpr int kNumLeds = 18;
constexpr uint16_t kMaxMilliwatts = 850;
constexpr unsigned long kResetHoldMs = 3000;
}  // namespace hw

namespace led_cfg {
// Solid finished colour for this long, then fall back to the idle effect.
constexpr unsigned long kFinishedSolidMs = 15UL * 60UL * 1000UL;
// Idle longer than this turns the bar off until the UI or a print wakes it.
constexpr unsigned long kStandbyAfterIdleMs = 120UL * 60UL * 1000UL;
constexpr uint8_t kDefaultBrightnessPercent = 50;
}  // namespace led_cfg

namespace mqtt_cfg {
constexpr int kPort = 8883;
constexpr const char* kUsername = "bblp";
// First reconnect gap; BambuMqttClient stretches this after failures.
constexpr unsigned long kReconnectIntervalMs = 1500;
// Re-request full status if the printer stays quiet after connect.
constexpr unsigned long kPushallIntervalMs = 45000;
// Periodic pushall while connected so stale stg_cur cannot latch.
constexpr unsigned long kPeriodicPushallMs = 60000;
}  // namespace mqtt_cfg

// ==== PrinterState.h ====
// Pure logic: turns a Bambu Lab "device/<serial>/report" MQTT payload's
// "print" object into a SystemState + progress telemetry. No Arduino/WiFi/
// FastLED dependency, so this is unit-testable on the host (see
// test/test_printer_state.cpp).
//
// Stage / lifecycle rules follow PrintSphere-knomi (cptkirki/PrintSphere fork):
// https://github.com/forbidden-kiwi/PrintSphere-knomi
//   - gcode_state is latched across partial MQTT diffs
//   - stg_cur is latched; idle markers (-1/255) do not wipe an active stage
//   - terminal gcode (IDLE/FINISH/FAILED/OFFLINE) ignores prep stages
//   - stage IDs from ha-bambulab / bambu_stage_label_from_id
enum class SystemState : uint8_t {
  Provisioning,
  WifiConnecting,
  MqttConnecting,
  Idle,
  Homing,
  Leveling,
  Downloading,
  Preparing,
  Heating,
  Printing,
  Paused,
  Finished,
  Error,
};

struct PrinterTelemetry {
  uint8_t progressPercent = 0;
  unsigned long finishedAtMs = 0;
};

class PrinterStateMachine {
 public:
  // Call once per received MQTT payload with doc["print"] and millis().
  // Returns the resulting state (also available via state()).
  SystemState update(ArduinoJson::JsonVariantConst printObj, unsigned long nowMs);

  SystemState state() const { return state_; }
  const PrinterTelemetry& telemetry() const { return telemetry_; }

 private:
  void updateHeating(ArduinoJson::JsonVariantConst printObj);
  void resolveFromLatched(unsigned long nowMs);

  SystemState state_ = SystemState::MqttConnecting;
  PrinterTelemetry telemetry_;

  // Latched fields (PrintSphere keeps these across partial reports).
  char gcodeState_[16] = "";
  int stgCur_ = -1;
  bool haveStgCur_ = false;
  uint8_t mcPercent_ = 0;
  bool haveMcPercent_ = false;
  uint16_t layerNum_ = 0;
  bool amsFilamentChange_ = false;
  int printErrorCode_ = 0;
  bool hasHmsAlert_ = false;

  float lastNozzleTemp_ = 0.0f;
  bool hasNozzleBaseline_ = false;
  bool isHeating_ = false;
  float heatingBaselineTemp_ = 0.0f;
  unsigned long lastHmsTimeMs_ = 0;
  // FINISH / stg_cur 255 stick on Bambu after a prior job. Only celebrate
  // Finished if we saw an active print in this MQTT session.
  bool sawActivePrint_ = false;
};

// ==== PrinterState.cpp ====
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonVariantConst;

namespace {
constexpr float kHeatingCandidateTargetC = 50.0f;
constexpr float kHeatingCandidateDeltaC = 3.0f;
constexpr float kCoolingDeltaC = -2.0f;
constexpr float kFallbackHeatingTargetC = 220.0f;  // used only if the payload never reports a target
constexpr float kIdleNozzleThresholdC = 50.0f;

bool equalsIgnoreCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
    if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
    if (ca != cb) return false;
  }
  return *a == *b;
}

bool gcodeIs(const char* state, const char* value) { return equalsIgnoreCase(state, value); }

bool readIntField(JsonVariantConst printObj, const char* key, int& out) {
  if (printObj[key].isNull()) return false;
  if (printObj[key].is<const char*>()) {
    out = atoi(printObj[key].as<const char*>());
    return true;
  }
  if (printObj[key].is<int>() || printObj[key].is<long>() || printObj[key].is<float>()) {
    out = printObj[key].as<int>();
    return true;
  }
  return false;
}

bool copyGcode(char* dest, size_t destLen, const char* src) {
  if (!src || !src[0] || destLen == 0) return false;
  size_t i = 0;
  for (; src[i] && i + 1 < destLen; ++i) dest[i] = src[i];
  dest[i] = '\0';
  return i > 0;
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

uint8_t mapRange(float v, float inLo, float inHi, float outLo, float outHi) {
  if (inHi - inLo < 0.01f) return static_cast<uint8_t>(outLo);
  float t = (v - inLo) / (inHi - inLo);
  return static_cast<uint8_t>(clampf(outLo + t * (outHi - outLo), outLo, outHi));
}

// PrintSphere: active vs terminal gcode_state (printer_client.cpp).
bool isActiveGcode(const char* gcode) {
  return gcodeIs(gcode, "RUNNING") || gcodeIs(gcode, "PREPARE") || gcodeIs(gcode, "PAUSE") ||
         gcodeIs(gcode, "PAUSED") || gcodeIs(gcode, "INIT") || gcodeIs(gcode, "SLICING");
}

bool isIdleStageMarker(int stgCur) { return stgCur == -1 || stgCur == 255; }

// PrintSphere status_resolver classifiers → LED SystemState.
// Stage IDs from bambu_stage_label_from_id / ha-bambulab.
enum class StageKind : uint8_t { None, Homing, Leveling, Heating, Preparing, Paused, Printing };

StageKind classifyStage(int stgCur) {
  switch (stgCur) {
    case 13:  // homing_toolhead
    case 59:  // homing_blade_holder
      return StageKind::Homing;

    case 1:   // auto_bed_leveling
    case 40:  // bed_level_high_temperature
    case 47:  // bed_level_phase_1
    case 48:  // bed_level_phase_2
    case 57:  // measuring_surface
      return StageKind::Leveling;

    case 2:   // heatbed_preheating
    case 7:   // heating_hotend
    case 49:  // heating_chamber
    case 54:  // waiting_for_heatbed_temperature
    case 58:  // thermal_preconditioning
      return StageKind::Heating;

    case 5:   // m400_pause
    case 6:   // paused_filament_runout
    case 16:  // paused_user
    case 17:  // paused_front_cover_falling
    case 20:  // paused_nozzle_temperature_malfunction
    case 21:  // paused_heat_bed_temperature_malfunction
    case 23:  // paused_skipped_step
    case 26:  // paused_ams_lost
    case 27:  // paused_low_fan_speed_heat_break
    case 28:  // paused_chamber_temperature_control_error
    case 30:  // paused_user_gcode
    case 32:  // paused_nozzle_filament_covered_detected
    case 33:  // paused_cutter_error
    case 34:  // paused_first_layer_error
    case 35:  // paused_nozzle_clog
      return StageKind::Paused;

    case 0:  // printing
      return StageKind::Printing;

    case 3:   // sweeping_xy_mech_mode
    case 4:   // changing_filament
    case 8:   // calibrating_extrusion
    case 9:   // scanning_bed_surface (PrintSphere: setup, not bed level)
    case 10:  // inspecting_first_layer
    case 11:  // identifying_build_plate_type
    case 12:  // calibrating_micro_lidar
    case 14:  // cleaning_nozzle_tip
    case 15:  // checking_extruder_temperature (setup, not preheat)
    case 18:  // calibrating_micro_lidar
    case 19:  // calibrating_extrusion_flow
    case 22:  // filament_unloading
    case 24:  // filament_loading
    case 25:  // calibrating_motor_noise
    case 29:  // cooling_chamber
    case 31:  // motor_noise_showoff
    case 36:  // check_absolute_accuracy_before_calibration
    case 37:  // absolute_accuracy_calibration
    case 38:  // check_absolute_accuracy_after_calibration
    case 39:  // calibrate_nozzle_offset
    case 41:  // check_quick_release
    case 42:  // check_door_and_cover
    case 43:  // laser_calibration
    case 44:  // check_plaform
    case 45:  // check_birdeye_camera_position
    case 46:  // calibrate_birdeye_camera
    case 50:  // heated_bedcooling
    case 51:  // print_calibration_lines
    case 52:  // check_material
    case 53:  // calibrating_live_view_camera
    case 55:  // check_material_position
    case 56:  // calibrating_cutter_model_offset
    case 60:  // calibrating_camera_offset
    case 61:  // calibrating_blade_holder_position
    case 62:  // hotend_pick_place_test
    case 63:  // waiting_chamber_temperature_equalize
    case 64:  // preparing_hotend
    case 65:  // calibrating_detection_nozzle_clumping
    case 66:  // purifying_chamber_air
    case 77:  // preparing_ams
      return StageKind::Preparing;

    default:
      return StageKind::None;
  }
}

bool isPrepLikeStage(StageKind kind) {
  return kind == StageKind::Homing || kind == StageKind::Leveling || kind == StageKind::Heating ||
         kind == StageKind::Preparing;
}

SystemState stageKindToState(StageKind kind) {
  switch (kind) {
    case StageKind::Homing:
      return SystemState::Homing;
    case StageKind::Leveling:
      return SystemState::Leveling;
    case StageKind::Heating:
      return SystemState::Heating;
    case StageKind::Preparing:
      return SystemState::Preparing;
    case StageKind::Paused:
      return SystemState::Paused;
    case StageKind::Printing:
      return SystemState::Printing;
    default:
      return SystemState::Idle;
  }
}
}  // namespace

SystemState PrinterStateMachine::update(JsonVariantConst printObj, unsigned long nowMs) {
  // --- Latch partial MQTT fields (PrintSphere printer_client) ---
  // HMS / print_error only update when the field is present (diff payloads).
  if (!printObj["hms"].isNull()) {
    JsonArrayConst hms = printObj["hms"].as<JsonArrayConst>();
    hasHmsAlert_ = !hms.isNull() && hms.size() > 0;
    if (hasHmsAlert_) lastHmsTimeMs_ = nowMs;
  }
  if (!printObj["print_error"].isNull()) {
    int code = 0;
    if (readIntField(printObj, "print_error", code)) printErrorCode_ = code;
  }

  if (!printObj["gcode_state"].isNull()) {
    const char* gs = printObj["gcode_state"] | "";
    copyGcode(gcodeState_, sizeof(gcodeState_), gs);
  }

  const uint16_t previousLayer = layerNum_;
  const uint8_t previousPercent = mcPercent_;
  if (!printObj["layer_num"].isNull()) {
    int layer = 0;
    if (readIntField(printObj, "layer_num", layer) && layer >= 0) {
      layerNum_ = static_cast<uint16_t>(layer);
    }
  }
  if (!printObj["mc_percent"].isNull()) {
    mcPercent_ = printObj["mc_percent"].as<uint8_t>();
    haveMcPercent_ = true;
    telemetry_.progressPercent = mcPercent_;
  }

  int amsStatus = -1;
  if (readIntField(printObj, "ams_status", amsStatus) && amsStatus >= 0) {
    // High byte 0x01 = AMS_STATUS_MAIN_FILAMENT_CHANGE (A1 often keeps stg_cur=0).
    amsFilamentChange_ = ((amsStatus >> 8) & 0xFF) == 0x01;
  }

  int incomingStage = -1;
  const bool stageInPayload = readIntField(printObj, "stg_cur", incomingStage);
  if (stageInPayload) {
    const bool active = isActiveGcode(gcodeState_);
    if (active && isIdleStageMarker(incomingStage)) {
      // Transient idle placeholders between real sub-states — keep latched stage.
    } else {
      stgCur_ = incomingStage;
      haveStgCur_ = true;
    }
  }

  // Synthesize changing_filament while AMS change is latched and stage is generic.
  int effectiveStage = haveStgCur_ ? stgCur_ : -1;
  if (amsFilamentChange_ && (effectiveStage == 0 || isIdleStageMarker(effectiveStage))) {
    effectiveStage = 4;
  }

  // Stale prep-stage invalidation (PrintSphere v1.6.1): once RUNNING and
  // layer/progress advance without a fresh stage field, drop latched prep.
  if (!stageInPayload && gcodeIs(gcodeState_, "RUNNING") && haveStgCur_) {
    const StageKind latched = classifyStage(effectiveStage);
    const bool layerAdvanced = layerNum_ > previousLayer && layerNum_ > 0;
    const bool progressAdvanced =
        haveMcPercent_ && mcPercent_ > previousPercent && previousPercent < 100;
    if (isPrepLikeStage(latched) && (layerAdvanced || progressAdvanced)) {
      stgCur_ = 0;
      effectiveStage = 0;
    }
  }

  resolveFromLatched(nowMs);

  // Download / file prepare percent (not in PrintSphere UI map but useful for LEDs).
  if (!printObj["gcode_file_prepare_percent"].isNull() && printObj["gcode_file_prepare_percent"].is<int>()) {
    int preparePercent = printObj["gcode_file_prepare_percent"].as<int>();
    const bool locked = state_ == SystemState::Printing || state_ == SystemState::Finished ||
                        state_ == SystemState::Homing || state_ == SystemState::Leveling ||
                        state_ == SystemState::Heating || state_ == SystemState::Paused ||
                        state_ == SystemState::Error;
    if (!locked && preparePercent >= 0 && preparePercent < 100) {
      state_ = SystemState::Downloading;
    } else if (!locked && preparePercent == 100 && state_ == SystemState::Idle) {
      state_ = SystemState::Preparing;
    }
  }

  if (!printObj["mc_print_sub_stage"].isNull() && printObj["mc_print_sub_stage"].is<int>() &&
      printObj["mc_print_sub_stage"].as<int>() == 5 && isActiveGcode(gcodeState_)) {
    state_ = SystemState::Leveling;
  }

  updateHeating(printObj);

  // Print / pause progress must always win over heating-derived percentages.
  // Otherwise nozzle deltas during RUNNING briefly look like 0/9/25% on the bar.
  if ((state_ == SystemState::Printing || state_ == SystemState::Paused ||
       state_ == SystemState::Finished) &&
      haveMcPercent_) {
    isHeating_ = false;
    telemetry_.progressPercent =
        state_ == SystemState::Finished ? static_cast<uint8_t>(100) : mcPercent_;
  }

  if (gcodeState_[0] == '\0' && !haveMcPercent_ && !isHeating_ && !haveStgCur_) {
    if (printObj["nozzle_temper"].is<float>() &&
        printObj["nozzle_temper"].as<float>() < kIdleNozzleThresholdC) {
      state_ = SystemState::Idle;
    }
  }

  return state_;
}

void PrinterStateMachine::resolveFromLatched(unsigned long nowMs) {
  const char* gcode = gcodeState_;
  const int stage = haveStgCur_ ? stgCur_ : -1;
  int effectiveStage = stage;
  if (amsFilamentChange_ && (effectiveStage == 0 || isIdleStageMarker(effectiveStage))) {
    effectiveStage = 4;
  }
  const StageKind kind = classifyStage(effectiveStage);
  const bool filamentStage = (effectiveStage == 4 || effectiveStage == 22 || effectiveStage == 24);
  // print_error during filament change is often a user prompt, not a fault.
  const bool concreteError = printErrorCode_ != 0 && !filamentStage;

  // Terminal lifecycle wins over stg_cur (PrintSphere).
  // User abort → gcode FAILED with print_error=0 and empty HMS → "ready"/Idle
  // (non_error_stop in PrintSphere status_resolver).
  if (gcodeIs(gcode, "FAILED")) {
    if (concreteError || hasHmsAlert_) {
      state_ = SystemState::Error;
      lastHmsTimeMs_ = nowMs;
    } else {
      state_ = SystemState::Idle;
      sawActivePrint_ = false;
    }
    return;
  }
  if (gcodeIs(gcode, "FINISH")) {
    if (sawActivePrint_) {
      state_ = SystemState::Finished;
      telemetry_.progressPercent = 100;
      telemetry_.finishedAtMs = nowMs;
    } else {
      state_ = SystemState::Idle;
    }
    return;
  }
  if (gcodeIs(gcode, "IDLE") || gcodeIs(gcode, "OFFLINE")) {
    // Concrete print_error while idle is still a fault; HMS alone is not
    // (PrintSphere keeps idle/ready and only warns HMS during an active job).
    if (concreteError) {
      state_ = SystemState::Error;
      lastHmsTimeMs_ = nowMs;
    } else {
      state_ = SystemState::Idle;
    }
    return;
  }

  // Hard LED error only on a real print_error (not HMS-only, not filament prompts).
  if (concreteError) {
    state_ = SystemState::Error;
    lastHmsTimeMs_ = nowMs;
    return;
  }

  if (gcodeIs(gcode, "PAUSE") || gcodeIs(gcode, "PAUSED")) {
    sawActivePrint_ = true;
    if (kind == StageKind::Preparing || kind == StageKind::Heating || kind == StageKind::Homing ||
        kind == StageKind::Leveling) {
      state_ = stageKindToState(kind);
    } else if (kind == StageKind::Paused) {
      state_ = SystemState::Paused;
    } else {
      state_ = SystemState::Paused;
    }
    return;
  }

  if (gcodeIs(gcode, "PREPARE") || gcodeIs(gcode, "INIT") || gcodeIs(gcode, "SLICING")) {
    sawActivePrint_ = true;
    if (isPrepLikeStage(kind) || kind == StageKind::Paused) {
      state_ = stageKindToState(kind);
    } else {
      state_ = SystemState::Preparing;
    }
    return;
  }

  if (gcodeIs(gcode, "RUNNING")) {
    sawActivePrint_ = true;
    if (isPrepLikeStage(kind)) {
      state_ = stageKindToState(kind);
    } else if (kind == StageKind::Paused) {
      state_ = SystemState::Paused;
    } else {
      state_ = SystemState::Printing;
      if (haveMcPercent_) telemetry_.progressPercent = mcPercent_;
    }
    return;
  }

  // No (or unknown) gcode_state — fall back to stage / progress like before.
  if (isPrepLikeStage(kind)) {
    state_ = stageKindToState(kind);
    return;
  }
  if (kind == StageKind::Paused) {
    state_ = SystemState::Paused;
    return;
  }
  if (kind == StageKind::Printing || (haveMcPercent_ && mcPercent_ > 0 && mcPercent_ < 100)) {
    state_ = SystemState::Printing;
    sawActivePrint_ = true;
    if (haveMcPercent_) telemetry_.progressPercent = mcPercent_;
    return;
  }
  if (isIdleStageMarker(stage) || stage == 0) {
    if (sawActivePrint_ && isIdleStageMarker(stage) && haveMcPercent_ && mcPercent_ >= 100) {
      state_ = SystemState::Finished;
      telemetry_.progressPercent = 100;
      telemetry_.finishedAtMs = nowMs;
    } else {
      state_ = SystemState::Idle;
    }
    return;
  }

  if (state_ == SystemState::MqttConnecting) {
    state_ = SystemState::Idle;
  }
}

void PrinterStateMachine::updateHeating(JsonVariantConst printObj) {
  if (printObj["nozzle_temper"].isNull()) return;
  float nozzleTemp = printObj["nozzle_temper"].as<float>();

  if (!hasNozzleBaseline_) {
    hasNozzleBaseline_ = true;
    lastNozzleTemp_ = nozzleTemp;
    return;
  }

  // Never invent heating progress once a job is live — stage flips
  // (clean nozzle / filament) must not rewrite the print percent bar.
  if (isActiveGcode(gcodeState_) || state_ == SystemState::Printing ||
      state_ == SystemState::Paused || state_ == SystemState::Finished ||
      (haveMcPercent_ && mcPercent_ > 0 && sawActivePrint_)) {
    isHeating_ = false;
    lastNozzleTemp_ = nozzleTemp;
    return;
  }

  float delta = nozzleTemp - lastNozzleTemp_;
  bool haveTarget = printObj["nozzle_target_temper"].is<float>();
  float target = haveTarget ? printObj["nozzle_target_temper"].as<float>() : kFallbackHeatingTargetC;

  bool allowInfer =
      state_ == SystemState::MqttConnecting || state_ == SystemState::Idle ||
      state_ == SystemState::WifiConnecting || state_ == SystemState::Homing ||
      state_ == SystemState::Preparing || state_ == SystemState::Leveling;
  bool candidateHeating =
      allowInfer && ((haveTarget && target > kHeatingCandidateTargetC && nozzleTemp < target - 5.0f) ||
                     (nozzleTemp >= 40.0f && delta >= kHeatingCandidateDeltaC));

  if (candidateHeating && !isHeating_) {
    isHeating_ = true;
    heatingBaselineTemp_ = lastNozzleTemp_;
    state_ = SystemState::Heating;
  }

  if (isHeating_) {
    float clamped = clampf(nozzleTemp, heatingBaselineTemp_, target);
    telemetry_.progressPercent = mapRange(clamped, heatingBaselineTemp_, target, 0.0f, 100.0f);

    bool tempReached = nozzleTemp >= target - 2.0f;
    bool cooling = delta < kCoolingDeltaC;
    if (tempReached || cooling) {
      isHeating_ = false;
      if (cooling || gcodeIs(gcodeState_, "IDLE"))
        state_ = SystemState::Idle;
      else
        state_ = SystemState::Preparing;
    }
  }

  lastNozzleTemp_ = nozzleTemp;
}

// ==== LedEffects.h ====
// Selectable LED animations for Idle / Printing / Error.
enum class LedEffect : uint8_t {
  Pulse = 0,
  Solid = 1,
  Comet = 2,
  Wave = 3,
  Strobe = 4,
  Progress = 5,
  Rainbow = 6,
  Sparkle = 7,
  Fire = 8,
};

inline constexpr uint8_t kLedEffectCount = 9;

inline const char* ledEffectName(LedEffect effect) {
  switch (effect) {
    case LedEffect::Pulse:
      return "pulse";
    case LedEffect::Solid:
      return "solid";
    case LedEffect::Comet:
      return "comet";
    case LedEffect::Wave:
      return "wave";
    case LedEffect::Strobe:
      return "strobe";
    case LedEffect::Progress:
      return "progress";
    case LedEffect::Rainbow:
      return "rainbow";
    case LedEffect::Sparkle:
      return "sparkle";
    case LedEffect::Fire:
      return "fire";
  }
  return "pulse";
}

inline bool parseLedEffect(const char* text, LedEffect& out) {
  if (!text || !text[0]) return false;
  if (!strcmp(text, "pulse")) {
    out = LedEffect::Pulse;
    return true;
  }
  if (!strcmp(text, "solid")) {
    out = LedEffect::Solid;
    return true;
  }
  if (!strcmp(text, "comet")) {
    out = LedEffect::Comet;
    return true;
  }
  if (!strcmp(text, "wave")) {
    out = LedEffect::Wave;
    return true;
  }
  if (!strcmp(text, "strobe")) {
    out = LedEffect::Strobe;
    return true;
  }
  if (!strcmp(text, "progress")) {
    out = LedEffect::Progress;
    return true;
  }
  if (!strcmp(text, "rainbow")) {
    out = LedEffect::Rainbow;
    return true;
  }
  if (!strcmp(text, "sparkle")) {
    out = LedEffect::Sparkle;
    return true;
  }
  if (!strcmp(text, "fire")) {
    out = LedEffect::Fire;
    return true;
  }
  return false;
}

inline LedEffect clampLedEffect(uint8_t raw, LedEffect fallback) {
  if (raw >= kLedEffectCount) return fallback;
  return static_cast<LedEffect>(raw);
}

// ==== LedController.h ====
struct LedStyleSlot {
  CRGB color;
  LedEffect effect;
};

struct LedStyle {
  uint8_t brightness = led_cfg::kDefaultBrightnessPercent;  // 0..100
  uint8_t speed = 50;                                       // 1..100, 50 is 1x
  LedStyleSlot idle{CRGB::White, LedEffect::Pulse};
  LedStyleSlot printing{CRGB::White, LedEffect::Progress};
  LedStyleSlot error{CRGB::Red, LedEffect::Strobe};
  LedStyleSlot homing{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot leveling{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot preparing{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot heating{CRGB(255, 80, 0), LedEffect::Progress};
  LedStyleSlot downloading{CRGB(255, 220, 0), LedEffect::Comet};
  LedStyleSlot paused{CRGB::White, LedEffect::Pulse};
  LedStyleSlot finished{CRGB::Green, LedEffect::Solid};
  bool reverse = false;  // flip physical LED order for mount orientation
};

class LedController {
 public:
  void begin();
  void setStyle(const LedStyle& style);
  void setAsleep(bool asleep) { asleep_ = asleep; }
  void render(SystemState state, const PrinterTelemetry& telemetry, unsigned long nowMs);

 private:
  void renderProgressBar(CRGB onColor, CRGB offColor, uint8_t percent);
  void renderComet(CRGB color, unsigned long nowMs, uint16_t stepMs);
  void renderWave(unsigned long nowMs, uint8_t hue);
  void renderWaveColor(CRGB color, unsigned long nowMs);
  void renderPulse(CRGB color, unsigned long nowMs, uint16_t periodMs);
  void renderStrobe(CRGB color, unsigned long nowMs, uint16_t onMs, uint16_t offMs);
  void renderSolid(CRGB color);
  void renderRainbow(unsigned long nowMs);
  void renderSparkle(CRGB color, unsigned long nowMs);
  void renderFire(unsigned long nowMs);
  void renderHeating(uint8_t percent);
  void renderEffect(LedEffect effect, CRGB color, uint8_t progressPercent, unsigned long nowMs);
  void renderSlot(const LedStyleSlot& slot, uint8_t progressPercent, unsigned long nowMs);
  void showBlack();
  void showLeds();
  uint16_t scaled(uint16_t baseMs) const;

  CRGB leds_[hw::kNumLeds];
  LedStyle style_;
  bool asleep_ = false;
  uint8_t heat_[hw::kNumLeds] = {};
};

// ==== LedController.cpp ====
namespace {
uint8_t brightnessToPwm(uint8_t percent) {
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255) / 100);
}
}  // namespace

void LedController::begin() {
  FastLED.addLeds<WS2812B, hw::kDataPin, GRB>(leds_, hw::kNumLeds);
  FastLED.setMaxPowerInMilliWatts(hw::kMaxMilliwatts);
  FastLED.setBrightness(brightnessToPwm(style_.brightness));
  FastLED.clear(true);
}

void LedController::setStyle(const LedStyle& style) {
  style_ = style;
  if (style_.speed < 1) style_.speed = 1;
  if (style_.speed > 100) style_.speed = 100;
  if (style_.brightness > 100) style_.brightness = 100;
  auto clampSlot = [](LedStyleSlot& slot, LedEffect fallback) {
    slot.effect = clampLedEffect(static_cast<uint8_t>(slot.effect), fallback);
  };
  clampSlot(style_.idle, LedEffect::Pulse);
  clampSlot(style_.printing, LedEffect::Progress);
  clampSlot(style_.error, LedEffect::Strobe);
  clampSlot(style_.homing, LedEffect::Comet);
  clampSlot(style_.leveling, LedEffect::Comet);
  clampSlot(style_.preparing, LedEffect::Comet);
  clampSlot(style_.heating, LedEffect::Progress);
  clampSlot(style_.downloading, LedEffect::Comet);
  clampSlot(style_.paused, LedEffect::Pulse);
  clampSlot(style_.finished, LedEffect::Solid);
  FastLED.setBrightness(brightnessToPwm(style_.brightness));
}

uint16_t LedController::scaled(uint16_t baseMs) const {
  uint8_t speed = style_.speed < 1 ? 1 : style_.speed;
  uint32_t period = (static_cast<uint32_t>(baseMs) * 50) / speed;
  if (period < 16) period = 16;
  if (period > 20000) period = 20000;
  return static_cast<uint16_t>(period);
}

void LedController::showLeds() {
  if (style_.reverse) {
    for (int i = 0, j = hw::kNumLeds - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
  FastLED.show();
  // Restore logical order so stateful effects (sparkle/fire) keep working.
  if (style_.reverse) {
    for (int i = 0, j = hw::kNumLeds - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
}

void LedController::showBlack() {
  fill_solid(leds_, hw::kNumLeds, CRGB::Black);
  showLeds();
}

void LedController::renderSolid(CRGB color) {
  fill_solid(leds_, hw::kNumLeds, color);
  showLeds();
}

void LedController::renderProgressBar(CRGB onColor, CRGB offColor, uint8_t percent) {
  int onLeds = static_cast<int>(hw::kNumLeds * (percent / 100.0f) + 0.5f);
  for (int i = 0; i < hw::kNumLeds; i++) {
    leds_[i] = (i < onLeds) ? onColor : offColor;
  }
  showLeds();
}

void LedController::renderHeating(uint8_t percent) {
  int lit = static_cast<int>(hw::kNumLeds * (percent / 100.0f) + 0.5f);
  fill_solid(leds_, hw::kNumLeds, CRGB(30, 0, 0));
  for (int i = 0; i < lit; i++) {
    leds_[i] = CRGB(map(i, 0, max(lit - 1, 1), 64, 255), map(i, 0, max(lit - 1, 1), 21, 85), 0);
  }
  showLeds();
}

void LedController::renderComet(CRGB color, unsigned long nowMs, uint16_t stepMs) {
  static int pos = 0;
  static int dir = 1;
  static unsigned long lastStep = 0;
  if (nowMs - lastStep > stepMs) {
    lastStep = nowMs;
    pos += dir;
    if (pos >= hw::kNumLeds - 1) dir = -1;
    if (pos <= 0) dir = 1;
  }
  fill_solid(leds_, hw::kNumLeds, CRGB::Black);
  for (int tail = 0; tail < 4; tail++) {
    int idx = pos - tail * dir;
    if (idx >= 0 && idx < hw::kNumLeds) leds_[idx] = color / (1 << tail);
  }
  showLeds();
}

void LedController::renderWave(unsigned long nowMs, uint8_t hue) {
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  for (int i = 0; i < hw::kNumLeds; i++) {
    uint8_t v = sin8(static_cast<uint8_t>(i * 16 + phase * 40));
    leds_[i] = CHSV(hue, 255, v);
  }
  showLeds();
}

void LedController::renderWaveColor(CRGB color, unsigned long nowMs) {
  // Scale the chosen RGB color — do not force HSV sat=255 (white → hue 0 = red).
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  for (int i = 0; i < hw::kNumLeds; i++) {
    uint8_t v = sin8(static_cast<uint8_t>(i * 16 + phase * 40));
    CRGB lit = color;
    lit.nscale8(v);
    leds_[i] = lit;
  }
  showLeds();
}

void LedController::renderPulse(CRGB color, unsigned long nowMs, uint16_t periodMs) {
  uint16_t t = nowMs % periodMs;
  float phase = (t < periodMs / 2) ? (t / static_cast<float>(periodMs / 2))
                                    : (1.0f - (t - periodMs / 2) / static_cast<float>(periodMs / 2));
  CRGB col = color;
  col.nscale8(static_cast<uint8_t>(32 + phase * 223));
  fill_solid(leds_, hw::kNumLeds, col);
  showLeds();
}

void LedController::renderStrobe(CRGB color, unsigned long nowMs, uint16_t onMs, uint16_t offMs) {
  uint16_t t = nowMs % (onMs + offMs);
  fill_solid(leds_, hw::kNumLeds, t < onMs ? color : CRGB::Black);
  showLeds();
}

void LedController::renderRainbow(unsigned long nowMs) {
  uint8_t speed = style_.speed < 1 ? 1 : style_.speed;
  uint8_t base = static_cast<uint8_t>((nowMs / (20 + (100 - speed))) & 0xFF);
  for (int i = 0; i < hw::kNumLeds; i++) {
    leds_[i] = CHSV(static_cast<uint8_t>(base + i * (256 / hw::kNumLeds)), 255, 255);
  }
  showLeds();
}

void LedController::renderSparkle(CRGB color, unsigned long nowMs) {
  static unsigned long lastStep = 0;
  fadeToBlackBy(leds_, hw::kNumLeds, 40);
  if (nowMs - lastStep >= scaled(40)) {
    lastStep = nowMs;
    int sparks = 1 + (style_.speed / 40);
    for (int s = 0; s < sparks; s++) {
      leds_[random16(hw::kNumLeds)] = color;
    }
  }
  showLeds();
}

void LedController::renderFire(unsigned long nowMs) {
  static unsigned long lastStep = 0;
  if (nowMs - lastStep < scaled(30)) {
    showLeds();
    return;
  }
  lastStep = nowMs;

  for (int i = 0; i < hw::kNumLeds; i++) {
    heat_[i] = qsub8(heat_[i], random8(0, 40));
  }
  for (int k = hw::kNumLeds - 1; k >= 2; k--) {
    heat_[k] = (heat_[k - 1] + heat_[k - 2] + heat_[k - 2]) / 3;
  }
  if (random8() < 120) {
    int y = random8(2);
    heat_[y] = qadd8(heat_[y], random8(160, 255));
  }
  for (int j = 0; j < hw::kNumLeds; j++) {
    leds_[j] = HeatColor(heat_[j]);
  }
  showLeds();
}

void LedController::renderEffect(LedEffect effect, CRGB color, uint8_t progressPercent, unsigned long nowMs) {
  switch (effect) {
    case LedEffect::Solid:
      renderSolid(color);
      break;
    case LedEffect::Comet:
      renderComet(color, nowMs, scaled(70));
      break;
    case LedEffect::Wave:
      renderWaveColor(color, nowMs);
      break;
    case LedEffect::Strobe:
      renderStrobe(color, nowMs, scaled(180), scaled(180));
      break;
    case LedEffect::Progress:
      renderProgressBar(color, CRGB::Black, progressPercent);
      break;
    case LedEffect::Rainbow:
      renderRainbow(nowMs);
      break;
    case LedEffect::Sparkle:
      renderSparkle(color, nowMs);
      break;
    case LedEffect::Fire:
      renderFire(nowMs);
      break;
    case LedEffect::Pulse:
    default:
      renderPulse(color, nowMs, scaled(3000));
      break;
  }
}

void LedController::renderSlot(const LedStyleSlot& slot, uint8_t progressPercent, unsigned long nowMs) {
  renderEffect(slot.effect, slot.color, progressPercent, nowMs);
}

void LedController::render(SystemState state, const PrinterTelemetry& telemetry, unsigned long nowMs) {
  FastLED.setBrightness(brightnessToPwm(style_.brightness));

  if (state == SystemState::Finished && telemetry.finishedAtMs != 0 &&
      nowMs - telemetry.finishedAtMs >= led_cfg::kFinishedSolidMs) {
    state = SystemState::Idle;
  }

  if (asleep_ && state == SystemState::Idle) {
    showBlack();
    return;
  }

  switch (state) {
    case SystemState::Provisioning:
      renderComet(CRGB::Blue, nowMs, scaled(60));
      break;
    case SystemState::WifiConnecting:
      renderWave(nowMs, 160);
      break;
    case SystemState::MqttConnecting:
      renderStrobe(CRGB::Blue, nowMs, scaled(120), scaled(120));
      break;
    case SystemState::Idle:
      renderSlot(style_.idle, 100, nowMs);
      break;
    case SystemState::Homing:
      renderSlot(style_.homing, 100, nowMs);
      break;
    case SystemState::Leveling:
      renderSlot(style_.leveling, 100, nowMs);
      break;
    case SystemState::Preparing:
      renderSlot(style_.preparing, 100, nowMs);
      break;
    case SystemState::Downloading:
      renderSlot(style_.downloading, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Heating:
      // Keep the classic heat ramp when Progress is selected; other effects use the slot color.
      if (style_.heating.effect == LedEffect::Progress) {
        renderHeating(telemetry.progressPercent);
      } else {
        renderSlot(style_.heating, telemetry.progressPercent, nowMs);
      }
      break;
    case SystemState::Printing:
      renderSlot(style_.printing, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Paused:
      renderSlot(style_.paused, telemetry.progressPercent, nowMs);
      break;
    case SystemState::Finished:
      renderSlot(style_.finished, 100, nowMs);
      break;
    case SystemState::Error:
      renderSlot(style_.error, 100, nowMs);
      break;
  }
}

// ==== BambuMqttClient.h ====
// Thin wrapper around PubSubClient for Bambu Lab's LAN-mode MQTT broker.
// Bambu's local broker uses a self-signed cert, so TLS verification is
// intentionally disabled (setInsecure()) — that's standard practice for
// LAN-only local connections to these printers, not an oversight.
class BambuMqttClient {
 public:
  using PayloadHandler = std::function<void(ArduinoJson::JsonVariantConst printObj, unsigned long nowMs)>;

  void begin(const String& host, uint16_t port, const String& username, const String& password,
             const String& serial);
  void setPayloadHandler(PayloadHandler handler) { handler_ = std::move(handler); }

  bool connected();
  void disconnect();
  void loop();
  // Non-blocking: attempts a (re)connect at most once per reconnect interval.
  void ensureConnected(unsigned long nowMs);

 private:
  static void staticCallback(char* topic, uint8_t* payload, unsigned int length);
  void handleMessage(uint8_t* payload, unsigned int length);
  void requestPushAll();

  WiFiClientSecure secureClient_;
  PubSubClient mqtt_{secureClient_};
  String host_;
  String username_;
  String password_;
  String serial_;
  uint16_t port_ = 8883;
  PayloadHandler handler_;
  unsigned long lastAttemptMs_ = 0;
  unsigned long lastPushAllMs_ = 0;
  unsigned long lastReportMs_ = 0;
  uint8_t failStreak_ = 0;

  static BambuMqttClient* instance_;
};

// ==== BambuMqttClient.cpp ====
BambuMqttClient* BambuMqttClient::instance_ = nullptr;

void BambuMqttClient::begin(const String& host, uint16_t port, const String& username, const String& password,
                             const String& serial) {
  host_ = host;
  port_ = port;
  username_ = username;
  password_ = password;
  serial_ = serial;

  if (mqtt_.connected()) mqtt_.disconnect();
  secureClient_.stop();

  instance_ = this;
  // Bambu's local broker uses a self-signed cert.
  secureClient_.setInsecure();
  // Keep TLS/MQTT attempts bounded so the web UI and LED loop stay responsive.
  secureClient_.setHandshakeTimeout(5);
  secureClient_.setTimeout(3000);
  mqtt_.setSocketTimeout(3);
  mqtt_.setBufferSize(MQTT_MAX_PACKET_SIZE);
  mqtt_.setKeepAlive(30);
  mqtt_.setServer(host_.c_str(), port_);
  mqtt_.setCallback(&BambuMqttClient::staticCallback);
  lastAttemptMs_ = 0;
  lastPushAllMs_ = 0;
  lastReportMs_ = 0;
  failStreak_ = 0;
}

bool BambuMqttClient::connected() { return mqtt_.connected(); }

void BambuMqttClient::disconnect() {
  if (mqtt_.connected()) mqtt_.disconnect();
  secureClient_.stop();
}

void BambuMqttClient::loop() {
  mqtt_.loop();
  if (!mqtt_.connected()) return;

  const unsigned long now = millis();
  // A1/P1 often stay quiet until pushall; also re-sync periodically so diff
  // reports cannot latch a stale stg_cur (PrintSphere kPeriodicPushallMs).
  const bool needPush = lastPushAllMs_ == 0 ||
                        (lastReportMs_ == 0 && now - lastPushAllMs_ > 4000) ||
                        (lastReportMs_ != 0 && now - lastReportMs_ > mqtt_cfg::kPushallIntervalMs) ||
                        (lastPushAllMs_ != 0 && now - lastPushAllMs_ > mqtt_cfg::kPeriodicPushallMs);
  if (needPush && (lastPushAllMs_ == 0 || now - lastPushAllMs_ > 3000)) {
    requestPushAll();
  }
}

void BambuMqttClient::requestPushAll() {
  if (!mqtt_.connected() || serial_.isEmpty()) return;
  const String topic = "device/" + serial_ + "/request";
  // pushall asks the printer for a full status snapshot (needed on A1/P1).
  const char* payload =
      "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
  if (mqtt_.publish(topic.c_str(), payload)) {
    lastPushAllMs_ = millis();
    Serial.println(F("MQTT pushall requested"));
  }
}

void BambuMqttClient::ensureConnected(unsigned long nowMs) {
  if (mqtt_.connected()) {
    failStreak_ = 0;
    return;
  }

  // Short gaps at first, then back off so a bad access code cannot starve the UI.
  unsigned long interval = mqtt_cfg::kReconnectIntervalMs;
  if (failStreak_ >= 8) interval = 20000;
  else if (failStreak_ >= 4) interval = 8000;
  else if (failStreak_ >= 2) interval = 3000;

  if (lastAttemptMs_ != 0 && nowMs - lastAttemptMs_ < interval) return;
  lastAttemptMs_ = nowMs;

  String clientId = "bambu-status-bar-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  Serial.print(F("MQTT connect "));
  Serial.print(host_);
  Serial.print(F(":"));
  Serial.println(port_);

  const bool ok = mqtt_.connect(clientId.c_str(), username_.c_str(), password_.c_str());
  if (ok) {
    failStreak_ = 0;
    lastReportMs_ = 0;
    lastPushAllMs_ = 0;
    String topic = "device/" + serial_ + "/report";
    mqtt_.subscribe(topic.c_str());
    Serial.println(F("MQTT connected"));
    requestPushAll();
  } else {
    if (failStreak_ < 250) failStreak_++;
    secureClient_.stop();
    Serial.print(F("MQTT failed, state="));
    Serial.println(mqtt_.state());
  }
}

void BambuMqttClient::staticCallback(char* /*topic*/, uint8_t* payload, unsigned int length) {
  if (instance_) instance_->handleMessage(payload, length);
}

void BambuMqttClient::handleMessage(uint8_t* payload, unsigned int length) {
  if (!handler_ || length == 0) return;

  // Bound the parse so oversized status dumps cannot blow the heap.
  ArduinoJson::JsonDocument doc;
  auto error = ArduinoJson::deserializeJson(doc, payload, length);
  if (error) return;

  ArduinoJson::JsonVariantConst printObj = doc["print"];
  if (printObj.isNull()) return;

  lastReportMs_ = millis();
  handler_(printObj, lastReportMs_);
}

// ==== Settings.h ====
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

// ==== Settings.cpp ====
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
  data_.hostname = prefs.getString("host", "BambuStatusBar");
  data_.printerIp = prefs.getString("ip", "");
  data_.printerSerial = prefs.getString("serial", "");
  data_.accessCode = prefs.getString("code", "");
  data_.apSsid = prefs.getString("apssid", "BambuStatusBar");
  data_.apPass = prefs.getString("appass", "987654321");
  data_.apIp = prefs.getString("apip", "192.168.254.1");
  data_.apEnabled = prefs.getUChar("apen", 1) != 0;
  data_.brightness = prefs.getUChar("bri", 50);
  data_.speed = prefs.getUChar("spd", 50);

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
  data_.language = prefs.getString("lang", "de");
  prefs.end();
  clamp();
  revision_++;
}

void SettingsStore::clamp() {
  if (data_.hostname.isEmpty()) data_.hostname = "BambuStatusBar";
  if (data_.apSsid.isEmpty()) data_.apSsid = "BambuStatusBar";
  if (data_.apPass.isEmpty()) data_.apPass = "987654321";
  if (data_.apIp.isEmpty()) data_.apIp = "192.168.254.1";
  if (data_.brightness > 100) data_.brightness = 100;
  if (data_.speed < 1) data_.speed = 1;
  if (data_.speed > 100) data_.speed = 100;
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
  data_.brightness = 50;
  data_.speed = 50;
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
  save();
}

bool SettingsStore::printerReady() const {
  return data_.printerIp.length() > 0 && data_.printerSerial.length() > 0 && data_.accessCode.length() > 0;
}

// ==== GithubOta.h ====
// Fetches the latest public GitHub Release and streams firmware.bin into the
// ESP32 OTA partition. No GitHub token — the repo must be public.
struct GithubReleaseInfo {
  bool ok = false;
  bool updateAvailable = false;
  String currentVersion;
  String latestVersion;
  String downloadUrl;
  String error;
};

class GithubOta {
 public:
  // Query api.github.com/.../releases/latest and compare tags to kFirmwareVersion.
  static GithubReleaseInfo checkLatest();

  // Stream-download downloadUrl into Update. Returns true on success (caller reboots).
  static bool installFromUrl(const String& downloadUrl, String& errorOut);
};

// ==== GithubOta.cpp ====
namespace {
constexpr const char* kUserAgent = "BambuStatusBar-OTA";

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool valid = false;
};

SemVer parseSemVer(const char* text) {
  SemVer v;
  if (!text || !text[0]) return v;
  if (text[0] == 'v' || text[0] == 'V') ++text;
  int parts[3] = {0, 0, 0};
  int idx = 0;
  const char* p = text;
  while (*p && idx < 3) {
    if (*p < '0' || *p > '9') break;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
      n = n * 10 + (*p - '0');
      ++p;
    }
    parts[idx++] = n;
    if (*p == '.') {
      ++p;
      continue;
    }
    break;
  }
  if (idx == 0) return v;
  v.major = parts[0];
  v.minor = parts[1];
  v.patch = parts[2];
  v.valid = true;
  return v;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

bool httpGetToString(const String& url, String& body, String& errorOut) {
  NetworkClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    errorOut = "http-begin";
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    errorOut = String("http-") + code;
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return true;
}
}  // namespace

GithubReleaseInfo GithubOta::checkLatest() {
  GithubReleaseInfo info;
  info.currentVersion = kFirmwareVersion;

  if (WiFi.status() != WL_CONNECTED) {
    info.error = "no-wifi";
    return info;
  }

  String url = String("https://api.github.com/repos/") + kGithubOwner + "/" + kGithubRepo +
               "/releases/latest";
  String body;
  if (!httpGetToString(url, body, info.error)) return info;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    info.error = "bad-json";
    return info;
  }

  const char* tag = doc["tag_name"] | "";
  if (!tag[0]) {
    info.error = "no-release";
    return info;
  }
  info.latestVersion = tag;
  if (info.latestVersion.startsWith("v") || info.latestVersion.startsWith("V")) {
    info.latestVersion.remove(0, 1);
  }

  const SemVer current = parseSemVer(kFirmwareVersion);
  const SemVer latest = parseSemVer(tag);
  if (!current.valid || !latest.valid) {
    info.error = "bad-version";
    return info;
  }
  info.updateAvailable = compareSemVer(latest, current) > 0;

  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    const char* name = asset["name"] | "";
    if (strcmp(name, kGithubAssetName) != 0) continue;
    const char* dl = asset["browser_download_url"] | "";
    if (dl[0]) {
      info.downloadUrl = dl;
      break;
    }
  }
  if (info.downloadUrl.isEmpty()) {
    info.error = "no-asset";
    return info;
  }

  info.ok = true;
  return info;
}

bool GithubOta::installFromUrl(const String& downloadUrl, String& errorOut) {
  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "no-wifi";
    return false;
  }
  if (downloadUrl.isEmpty()) {
    errorOut = "no-url";
    return false;
  }

  NetworkClientSecure client;
  client.setInsecure();
  client.setTimeout(30000);

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(30000);
  if (!http.begin(client, downloadUrl)) {
    errorOut = "http-begin";
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    errorOut = String("http-") + code;
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  WiFiClient& stream = http.getStream();
  if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN)) {
    errorOut = "update-begin";
    http.end();
    return false;
  }

  const size_t written = Update.writeStream(stream);
  if (contentLength > 0 && written != static_cast<size_t>(contentLength)) {
    Update.abort();
    errorOut = "short-write";
    http.end();
    return false;
  }
  if (!Update.end(true)) {
    errorOut = "update-end";
    http.end();
    return false;
  }
  http.end();
  return true;
}

// ==== WebPage.h ====
// Bambu Status Bar web UI: hero, status badges, collapsible sections,
// amber primary actions. APIs stay local to this Status Bar firmware.
const char INDEX_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bambu Status Bar</title>
<style>
:root{--bg:#0b1015;--panel:#121a23;--panel-2:#172230;--line:#263548;--text:#eef4fb;--muted:#99a9bb;--accent:#f0a64b;--accent-2:#42c291;--warn:#f5c24f;--danger:#ff6b6b}
*{box-sizing:border-box}html,body{margin:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:radial-gradient(circle at top,#182433 0,#0b1015 55%);color:var(--text);padding:24px}
main{max-width:1040px;margin:0 auto;display:grid;gap:18px}
h1,h2,h3,p{margin:0}label{display:block;margin:0 0 8px;font-size:14px;font-weight:600;color:#d5e1ed}
input,select{width:100%;padding:13px 14px;border-radius:14px;border:1px solid var(--line);background:#0f1721;color:var(--text);outline:none;font:inherit}
input:focus,select:focus{border-color:#4d86c7;box-shadow:0 0 0 3px rgba(77,134,199,.18)}
input[type=color]{min-height:54px;padding:6px 10px;background:#0c131b}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;background:#1e3347;border-radius:3px;border:none;padding:0;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:18px;height:18px;border-radius:50%;background:#94a3b8}
input[type=checkbox]{width:auto}
button{border:none;border-radius:999px;padding:13px 18px;font-weight:700;cursor:pointer;font:inherit}
button:disabled{opacity:.65;cursor:wait}
button.primary{background:linear-gradient(135deg,#f0a64b,#ffd07a);color:#1b1203}
button.secondary{background:#203044;color:var(--text);border:1px solid #34506e}
.hero,.section,.footer-card{background:rgba(18,26,35,.94);border:1px solid var(--line);border-radius:24px;box-shadow:0 14px 32px rgba(0,0,0,.2)}
.hero{padding:24px;display:grid;gap:18px}
.section{padding:0;overflow:hidden}
.section-body{padding:0 22px 22px;display:grid;gap:16px}
.footer-card{padding:18px;display:grid;gap:8px}
.stack{display:grid;gap:18px}
.hero-top{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
.hero-brand{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.eyebrow{font-size:18px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;color:#b9d4ef}
.hero-version{display:inline-flex;align-items:center;justify-content:center;padding:6px 12px;border-radius:999px;border:1px solid #31557d;background:#111b29;color:#dceafa;font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;text-decoration:none}
.title{font-size:34px;line-height:1.05}.subtitle{max-width:720px;color:var(--muted);line-height:1.55}
.micro,.section-summary p{color:var(--muted);line-height:1.5}.micro{font-size:13px}
.section-summary{display:grid;grid-template-columns:1fr auto;gap:6px 14px;align-items:center;padding:22px;cursor:pointer;list-style:none}
.section-summary::-webkit-details-marker{display:none}
.section-summary h2{grid-column:1;grid-row:1;margin:0;font-size:22px}
.section-summary p{grid-column:1/-1;grid-row:2;margin:0}
.section-summary-side{grid-column:2;grid-row:1;display:flex;align-items:center;gap:10px}
.summary-pill{display:inline-flex;align-items:center;justify-content:center;padding:8px 12px;border-radius:999px;border:1px solid var(--line);font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;background:#0f1721;color:var(--text);white-space:nowrap}
.summary-pill.ok{border-color:#22614c;background:#10231d}
.summary-pill.warn{border-color:#7d6222;background:#241c0e}
.summary-pill.info{border-color:#31557d;background:#111b29}
.summary-pill.idle{border-color:#334456;background:#121a23}
.section-toggle-icon{width:32px;height:32px;border-radius:999px;border:1px solid #365064;background:#0f1721;display:grid;place-items:center;flex:0 0 auto}
.section-toggle-icon::before{content:'+';font-size:20px;line-height:1;color:#cfe0f1}
details[open]>.section-summary .section-toggle-icon::before{content:'-'}
.hint-box{padding:14px 16px;border-radius:18px;background:#0e1620;border:1px solid #2b3e54;color:var(--muted)}
.hint-box strong{color:var(--text)}
.badge-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
.badge{padding:14px 16px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:6px}
.badge-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91a6bc}
.badge-value{font-size:15px;font-weight:700;line-height:1.35}
.badge.ok{border-color:#22614c;background:#10231d}
.badge.warn{border-color:#7d6222;background:#241c0e}
.badge.info{border-color:#31557d;background:#111b29}
.badge.idle{border-color:#334456;background:#121a23}
.grid-2{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,460px),1fr));gap:14px}
.grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}
.field{display:grid;gap:8px}
.actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center}
.actions .micro{min-width:180px}
.color-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}
.light-rows{display:grid;gap:12px;margin-top:8px}
.light-row{display:grid;grid-template-columns:140px minmax(0,1fr);gap:12px;align-items:end}
@media(max-width:640px){.light-row{grid-template-columns:1fr}}
.check{display:flex;align-items:center;gap:10px;margin:0}
.status-line{font-size:16px;font-weight:700}
.status-detail{color:var(--muted)}
.printer-card{padding:16px 18px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:8px;margin-bottom:10px;cursor:pointer;text-align:left;width:100%;color:inherit;font:inherit}
.printer-card:hover{border-color:#4d86c7}
.ota-bar{display:none;margin:4px 0;height:8px;border-radius:6px;background:#0e1620;overflow:hidden}
.ota-bar>i{display:block;height:100%;width:0;background:var(--accent);transition:width .25s}
@media(max-width:840px){.badge-grid,.color-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:640px){body{padding:16px}.title{font-size:28px}.badge-grid,.grid-2,.grid-3,.color-grid{grid-template-columns:1fr}.hero,.footer-card{padding:18px}.section-summary{padding:18px}.section-summary-side{width:100%;justify-content:space-between}.section-body{padding:0 18px 18px}}
</style>
</head>
<body>
<main>
<section class="hero">
  <div class="hero-top">
    <div class="hero-brand">
      <div class="eyebrow">Status Bar</div>
      <span class="hero-version" id="heroVersion">…</span>
    </div>
  </div>
  <h1 class="title" data-i="title">Bambu Status Bar</h1>
  <p class="subtitle" id="subtitle" data-i="subtitle">WLAN, lokaler Drucker und LED-Leiste für deinen Bambu Lab Drucker.</p>
  <div class="badge-grid">
    <div class="badge idle" id="wifiBadge"><div class="badge-label" data-i="badge_wifi">Wi-Fi</div><div class="badge-value" id="wifiBadgeVal">…</div></div>
    <div class="badge idle" id="localBadge"><div class="badge-label" data-i="badge_local">Local Path</div><div class="badge-value" id="localBadgeVal">…</div></div>
    <div class="badge idle" id="stateBadge"><div class="badge-label" data-i="badge_state">Status</div><div class="badge-value" id="stateBadgeVal">…</div></div>
    <div class="badge idle" id="apBadge"><div class="badge-label" data-i="badge_ap">Hotspot</div><div class="badge-value" id="apBadgeVal">…</div></div>
  </div>
  <div class="hint-box" id="heroHint"><strong data-i="note">Note:</strong> <span id="heroHintText"></span></div>
</section>

<form class="stack" id="configForm" onsubmit="return false">
<details class="section" id="secWifi" open>
  <summary class="section-summary">
    <h2 data-i="sec_wifi">Wi-Fi</h2>
    <p data-i="sec_wifi_desc">Dieses Netzwerk trägt die LED-Leiste und den lokalen Druckerzugriff.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="wifiPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="actions">
      <button type="button" class="secondary" id="wifiScan" data-i="scan">Scan Networks</button>
      <div class="micro" id="wifiScanDetail" data-i="scan_hint">Scan visible networks or type an SSID manually.</div>
    </div>
    <div class="field" id="wifiSelectField" style="display:none"><label for="wifiSelect" data-i="detected">Detected Networks</label><select id="wifiSelect"><option value="">…</option></select></div>
    <div class="grid-2">
      <div class="field"><label for="ssid" data-i="ssid">Wi-Fi SSID</label><input id="ssid" maxlength="32" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label for="wifiPass" data-i="password">Wi-Fi Password</label><input id="wifiPass" type="password" maxlength="64" autocomplete="off" placeholder=""></div>
    </div>
    <div class="grid-2">
      <div class="field"><label for="host" data-i="hostname">Hostname</label><input id="host" maxlength="32" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label>&nbsp;</label><button type="button" class="secondary" id="hostBtn" data-i="set_hostname">Set Hostname</button></div>
    </div>
    <p class="micro" data-i="hostname_hint">Fritzbox-safe: underscores become hyphens.</p>
    <p class="micro"><span data-i="wifi_status_label">Current network status:</span> <span id="wifiDetail">…</span></p>
    <div class="actions">
      <button type="button" class="primary" id="wifiBtn" data-i="connect">Connect Wi-Fi</button>
      <div class="micro" data-i="wifi_connect_hint">Saves credentials and starts the join immediately. The setup hotspot stays available if the join fails.</div>
    </div>
  </div>
</details>

<details class="section" id="secPrinter">
  <summary class="section-summary">
    <h2 data-i="sec_printer">Local Printer Path</h2>
    <p data-i="sec_printer_desc">Lokales MQTT liefert Live-Status, Fortschritt und Heizphasen für die LED-Leiste.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="localPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="actions">
      <button type="button" class="secondary" id="printerScan" data-i="printer_scan">Scan Printers</button>
      <div class="micro" id="printerScanDetail" data-i="printer_scan_hint">SSDP search on the LAN. Needs Wi-Fi first.</div>
    </div>
    <div id="printerList"></div>
    <div class="grid-2">
      <div class="field"><label for="printerIp" data-i="printer_ip">Printer IP or Hostname</label><input id="printerIp" maxlength="40" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label for="printerSerial" data-i="serial">Printer Serial Number</label><input id="printerSerial" maxlength="32" autocapitalize="characters" autocomplete="off"></div>
    </div>
    <div class="field"><label for="printerCode" data-i="code">Access Code</label><input id="printerCode" type="password" maxlength="16" autocomplete="off"></div>
    <div class="hint-box"><strong data-i="local_status">Local Status:</strong> <span id="mqttDetail">…</span></div>
    <div class="actions">
      <button type="button" class="primary" id="bindBtn" data-i="bind">Connect Local</button>
      <div class="micro" data-i="bind_hint">Saves printer credentials and reconnects MQTT without reboot.</div>
    </div>
  </div>
</details>

<details class="section" id="secLight">
  <summary class="section-summary">
    <h2 data-i="sec_light">Light Colors</h2>
    <p data-i="sec_light_desc">Helligkeit, Geschwindigkeit und Statusfarben der LED-Leiste. Änderungen gelten sofort.</p>
    <div class="section-summary-side"><span class="summary-pill info" id="lightPill" data-i="pill_live">Live</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <p class="micro" data-i="light_hint">Farbe und Animation pro Status. Helligkeit und Geschwindigkeit gelten für alle. Fertig bleibt 15 Minuten, danach Leerlauf.</p>
    <div class="grid-2">
      <div class="field"><label for="brightness"><span data-i="light_brightness">Brightness</span> <span id="briVal"></span></label><input id="brightness" type="range" min="0" max="100" value="50"></div>
      <div class="field"><label for="speed"><span data-i="light_speed">Speed</span> <span id="spdVal"></span></label><input id="speed" type="range" min="1" max="100" value="50"></div>
    </div>
    <label class="check"><input id="ledReverse" type="checkbox"> <span data-i="light_reverse">Reverse LED direction</span></label>
    <div class="light-rows" id="lightStates"></div>
    <div class="actions"><button type="button" class="secondary" id="lightReset" data-i="light_reset">Reset Colors</button></div>
  </div>
</details>

<details class="section" id="secAp">
  <summary class="section-summary">
    <h2 data-i="sec_ap">Setup Hotspot</h2>
    <p data-i="sec_ap_desc">Fallback-Access-Point, falls kein WLAN erreichbar ist.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="apPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <label class="check"><input id="apEnabled" type="checkbox"> <span data-i="ap_enable">Keep hotspot enabled when Wi-Fi is up</span></label>
    <div class="grid-2">
      <div class="field"><label for="apSsid" data-i="ap_ssid">Hotspot Name</label><input id="apSsid" maxlength="32" autocapitalize="off"></div>
      <div class="field"><label for="apPass" data-i="ap_pass">Hotspot Password</label><input id="apPass" maxlength="64" autocomplete="off"></div>
    </div>
    <div class="field"><label for="apIp" data-i="ap_ip">Hotspot IP</label><input id="apIp" maxlength="15" inputmode="decimal"></div>
    <div class="hint-box" data-i="ap_hint">Without Wi-Fi the hotspot stays on even if disabled here. Empty password = open, otherwise at least 8 characters.</div>
    <div class="actions">
      <button type="button" class="primary" id="apBtn" data-i="ap_save">Save + Restart</button>
      <div class="micro" data-i="ap_save_hint">Applies hotspot settings on the next boot.</div>
    </div>
  </div>
</details>

<details class="section" id="secOta">
  <summary class="section-summary">
    <h2 data-i="sec_ota">Firmware Update</h2>
    <p data-i="sec_ota_desc">Update von GitHub Releases oder per lokaler .bin-Datei.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="otaPill" data-i="pill_ota">OTA</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="hint-box"><strong data-i="warn">Warning:</strong> <span data-i="ota_hint">Das Gerät startet nach dem Flash neu. Nutze nur firmware.bin (ESP32-C6), nicht firmware.factory.bin.</span></div>
    <div class="field"><label data-i="ota_github">GitHub Update</label>
      <div class="micro" id="otaVersionInfo">—</div>
    </div>
    <div class="actions">
      <button type="button" class="secondary" id="otaCheckBtn" data-i="ota_check">Auf Update prüfen</button>
      <button type="button" class="primary" id="otaInstallBtn" data-i="ota_install" disabled>Update installieren</button>
    </div>
    <div class="micro" id="otaGhStatus" data-i="ota_gh_ready">Prüft die neueste Release auf GitHub.</div>
    <hr style="border:none;border-top:1px solid #243447;margin:18px 0">
    <div class="field"><label for="fw" data-i="ota_file">Firmware .bin Datei</label><input id="fw" type="file" accept=".bin,application/octet-stream"></div>
    <div class="ota-bar" id="otaProg"><i id="otaBar"></i></div>
    <div class="actions">
      <button type="button" class="secondary" id="otaBtn" data-i="ota_start">Hochladen &amp; flashen</button>
      <div class="micro" id="otaStatus" data-i="ota_ready">Oder lokal eine .bin für ESP32-C6 wählen.</div>
    </div>
  </div>
</details>

<details class="section" id="secDevice">
  <summary class="section-summary">
    <h2 data-i="sec_device">Device</h2>
    <p data-i="sec_device_desc">Sprache und Werkseinstellungen.</p>
    <div class="section-summary-side"><span class="summary-pill info" data-i="pill_device">Device</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="field"><label for="language" data-i="lang">Language</label>
      <select id="language"><option value="de">Deutsch</option><option value="en">English</option></select>
    </div>
    <div class="actions">
      <button type="button" class="secondary" id="resetBtn" data-i="factory">Factory Reset</button>
      <div class="micro" data-i="boot_hint">Or hold BOOT for 3 seconds while the device is already running.</div>
    </div>
  </div>
</details>

<section class="footer-card">
  <div class="status-line" id="status">…</div>
  <div class="status-detail" id="statusDetail"></div>
</section>
</form>
</main>
<script>
const I18N={
de:{
title:"Bambu Status Bar",subtitle:"WLAN, lokaler Drucker und LED-Leiste für deinen Bambu Lab Drucker.",
note:"Hinweis:",hint_ap:"Im Setup-Hotspot reicht zuerst Wi-Fi. Danach öffnest du diese Seite im Heimnetz.",
hint_ok:"Wi-Fi ist online. Druckerbindung, Licht und OTA kannst du unten einstellen.",
badge_wifi:"Wi-Fi",badge_local:"Local Path",badge_state:"Status",badge_ap:"Hotspot",
sec_wifi:"Wi-Fi",sec_wifi_desc:"Dieses Netzwerk trägt die LED-Leiste und den lokalen Druckerzugriff.",
scan:"Netzwerke suchen",scan_hint:"Sichtbar scannen oder SSID manuell eingeben.",detected:"Gefundene Netzwerke",
ssid:"Wi-Fi SSID",password:"Wi-Fi Passwort",hostname:"Hostname",set_hostname:"Hostname setzen",
hostname_hint:"Unterstriche → Bindestriche. Die Fritzbox behält den alten Namen oft sticky: Heimnetz → Netzwerk → Gerät entfernen, dann neu verbinden.",
wifi_status_label:"Aktueller Netzwerkstatus:",connect:"Wi-Fi verbinden",
wifi_connect_hint:"Speichert die Zugangsdaten und startet die Verbindung sofort. Der Setup-Hotspot bleibt bei Fehler verfügbar.",
sec_printer:"Local Printer Path",sec_printer_desc:"Lokales MQTT liefert Live-Status, Fortschritt und Heizphasen für die LED-Leiste.",
printer_scan:"Drucker suchen",printer_scan_hint:"Lauscht ~12 s auf Bambu-NOTIFY-Broadcasts (UDP 2021). Zuerst Wi-Fi verbinden.",
printer_ip:"Drucker-IP oder Hostname",serial:"Seriennummer",code:"Access Code",
local_status:"Local Status:",bind:"Lokal verbinden",bind_hint:"Speichert die Druckerdaten und verbindet MQTT ohne Neustart.",
sec_light:"Light Colors",sec_light_desc:"Helligkeit, Geschwindigkeit, Animationen und Statusfarben. Änderungen gelten sofort.",
pill_live:"Live",light_brightness:"Helligkeit",light_speed:"Geschwindigkeit",
light_idle:"Leerlauf",light_print:"Druck",light_error:"Fehler",
light_homing:"Referenzfahrt",light_leveling:"Nivellieren",light_preparing:"Vorbereitung",
light_heating:"Heizen",light_downloading:"Download",light_paused:"Pause",light_finished:"Fertig",
light_idle_fx:"Animation",light_print_fx:"Animation",light_error_fx:"Animation",
light_homing_fx:"Animation",light_leveling_fx:"Animation",light_preparing_fx:"Animation",
light_heating_fx:"Animation",light_downloading_fx:"Animation",light_paused_fx:"Animation",light_finished_fx:"Animation",
light_reverse:"LED-Richtung umkehren",
fx_pulse:"Atmen",fx_solid:"Fest",fx_comet:"Komet",fx_wave:"Welle",fx_strobe:"Blinken",fx_progress:"Fortschritt",
fx_rainbow:"Regenbogen",fx_sparkle:"Funkeln",fx_fire:"Feuer",
light_reset:"Farben & Animationen zurücksetzen",light_hint:"Farbe und Animation pro Status. Helligkeit und Geschwindigkeit gelten für alle. Fertig bleibt 15 Minuten, danach Leerlauf.",
sec_ap:"Setup Hotspot",sec_ap_desc:"Fallback-Access-Point, falls kein WLAN erreichbar ist.",
ap_enable:"Hotspot auch bei verbundenem Wi-Fi anlassen",ap_ssid:"Hotspot-Name",ap_pass:"Hotspot-Passwort",ap_ip:"Hotspot-IP",
ap_hint:"Ohne Wi-Fi bleibt der Hotspot an, auch wenn er hier aus ist. Passwort leer = offen, sonst mindestens 8 Zeichen.",
ap_save:"Speichern + Neustart",ap_save_hint:"Hotspot-Einstellungen gelten nach dem Neustart.",
sec_ota:"Firmware Update",sec_ota_desc:"Update von GitHub Releases oder per lokaler .bin-Datei.",
pill_ota:"OTA",warn:"Warnung:",ota_file:"Firmware-.bin-Datei",ota_start:"Hochladen & flashen",
ota_hint:"Das Gerät startet nach dem Flash neu. Nutze nur firmware.bin (ESP32-C6), nicht firmware.factory.bin.",
ota_ready:"Oder lokal eine .bin für ESP32-C6 wählen.",
ota_github:"GitHub Update",ota_check:"Auf Update prüfen",ota_install:"Update installieren",
ota_gh_ready:"Prüft die neueste Release auf GitHub.",
ota_up_to_date:"Du bist auf dem neuesten Stand.",ota_available:"Update verfügbar.",
ota_checking:"Prüfe GitHub…",ota_installing:"Lade Firmware und flashe…",
err_no_wifi:"Zuerst mit Wi-Fi verbinden",err_no_release:"Kein Release gefunden",err_no_asset:"Kein firmware.bin im Release",
err_http_begin:"Verbindung fehlgeschlagen",err_update_begin:"OTA konnte nicht starten",err_update_end:"OTA fehlgeschlagen",
err_short_write:"Download unvollständig",err_bad_version:"Versionsformat ungültig",err_bad_json:"Ungültige Antwort",
sec_device:"Device",sec_device_desc:"Sprache und Werkseinstellungen.",pill_device:"Device",lang:"Sprache",
factory:"Werkseinstellungen",boot_hint:"Oder BOOT am laufenden Gerät 3 Sekunden halten.",
wifi_on:"Verbunden",wifi_off:"Nicht verbunden",wifi_try:"Verbinde…",wifi_fail:"Wi-Fi fehlgeschlagen",
mqtt_on:"MQTT verbunden",mqtt_off:"Nicht verbunden",mqtt_wait:"Warte auf Druckerdaten",
ap_on:"Aktiv",ap_off:"Aus",saved:"Gespeichert",rebooting:"Neustart… Seite danach neu laden.",
ota_ok:"Update fertig, Neustart…",ota_fail:"Update fehlgeschlagen",
factory_ask:"Alle Einstellungen löschen und neu starten?",
state_provisioning:"Nicht eingerichtet",state_wifi:"Suche Wi-Fi",state_mqtt:"Verbinde Drucker",
state_idle:"Leerlauf",state_homing:"Referenzfahrt",state_leveling:"Nivellieren",state_preparing:"Vorbereitung",
state_downloading:"Download",state_heating:"Heizen",state_printing:"Druck",state_paused:"Pause",
state_finished:"Fertig",state_error:"Fehler",state_standby:"Standby",state_unknown:"—",
err_bad_ssid:"SSID ungültig",err_bad_pass:"Passwort ungültig",err_bad_json:"Ungültige Eingabe",
err_ap_pass:"Hotspot-Passwort: leer oder ≥ 8 Zeichen",err_bad_ip:"IP ungültig",
err_bad_host:"Hostname: Buchstaben, Zahlen, Bindestrich oder Unterstrich",err_bad_printer:"Druckerdaten ungültig",
err_bad_leds:"Lichtwerte ungültig"
},
en:{
title:"Bambu Status Bar",subtitle:"Wi-Fi, local printer path and LED bar for your Bambu Lab printer.",
note:"Note:",hint_ap:"While the setup hotspot is up, start with Wi-Fi. Then reopen this page on the home-network IP.",
hint_ok:"Wi-Fi is up. Review printer binding, light colors and OTA below.",
badge_wifi:"Wi-Fi",badge_local:"Local Path",badge_state:"Status",badge_ap:"Hotspot",
sec_wifi:"Wi-Fi",sec_wifi_desc:"This network carries the LED bar and the local printer path.",
scan:"Scan Networks",scan_hint:"Scan for visible networks or type an SSID manually.",detected:"Detected Networks",
ssid:"Wi-Fi SSID",password:"Wi-Fi Password",hostname:"Hostname",set_hostname:"Set Hostname",
hostname_hint:"Underscores become hyphens. Fritzbox often keeps the first name: Heimnetz → Netzwerk → remove device, then reconnect.",
wifi_status_label:"Current network status:",connect:"Connect Wi-Fi",
wifi_connect_hint:"Saves credentials and starts the join immediately. The setup hotspot stays available if the join fails.",
sec_printer:"Local Printer Path",sec_printer_desc:"Local MQTT provides live status, progress and heating phases for the LED bar.",
printer_scan:"Scan Printers",printer_scan_hint:"Listens ~12 s for Bambu NOTIFY broadcasts (UDP 2021). Needs Wi-Fi first.",
printer_ip:"Printer IP or Hostname",serial:"Printer Serial Number",code:"Access Code",
local_status:"Local Status:",bind:"Connect Local",bind_hint:"Saves printer credentials and reconnects MQTT without reboot.",
sec_light:"Light Colors",sec_light_desc:"Brightness, speed, animations and status colors. Changes apply live.",
pill_live:"Live",light_brightness:"Brightness",light_speed:"Speed",
light_idle:"Idle",light_print:"Printing",light_error:"Error",
light_homing:"Homing",light_leveling:"Leveling",light_preparing:"Preparing",
light_heating:"Heating",light_downloading:"Downloading",light_paused:"Paused",light_finished:"Finished",
light_idle_fx:"Animation",light_print_fx:"Animation",light_error_fx:"Animation",
light_homing_fx:"Animation",light_leveling_fx:"Animation",light_preparing_fx:"Animation",
light_heating_fx:"Animation",light_downloading_fx:"Animation",light_paused_fx:"Animation",light_finished_fx:"Animation",
light_reverse:"Reverse LED direction",
fx_pulse:"Pulse",fx_solid:"Solid",fx_comet:"Comet",fx_wave:"Wave",fx_strobe:"Strobe",fx_progress:"Progress",
fx_rainbow:"Rainbow",fx_sparkle:"Sparkle",fx_fire:"Fire",
light_reset:"Reset colors & animations",light_hint:"Color and animation per status. Brightness and speed apply to all. Finished stays for 15 minutes, then idle.",
sec_ap:"Setup Hotspot",sec_ap_desc:"Fallback access point when home Wi-Fi is unavailable.",
ap_enable:"Keep hotspot enabled when Wi-Fi is up",ap_ssid:"Hotspot Name",ap_pass:"Hotspot Password",ap_ip:"Hotspot IP",
ap_hint:"Without Wi-Fi the hotspot stays on even if disabled here. Empty password = open, otherwise at least 8 characters.",
ap_save:"Save + Restart",ap_save_hint:"Hotspot settings apply on the next boot.",
sec_ota:"Firmware Update",sec_ota_desc:"Update from GitHub Releases or a local .bin file.",
pill_ota:"OTA",warn:"Warning:",ota_file:"Firmware .bin file",ota_start:"Upload & Flash",
ota_hint:"The device restarts after flashing. Use firmware.bin (ESP32-C6) only — not firmware.factory.bin.",
ota_ready:"Or pick a local .bin built for ESP32-C6.",
ota_github:"GitHub update",ota_check:"Check for update",ota_install:"Install update",
ota_gh_ready:"Checks the latest release on GitHub.",
ota_up_to_date:"You are up to date.",ota_available:"Update available.",
ota_checking:"Checking GitHub…",ota_installing:"Downloading and flashing…",
err_no_wifi:"Connect to Wi-Fi first",err_no_release:"No release found",err_no_asset:"No firmware.bin in release",
err_http_begin:"Connection failed",err_update_begin:"OTA could not start",err_update_end:"OTA failed",
err_short_write:"Incomplete download",err_bad_version:"Invalid version format",err_bad_json:"Invalid response",
sec_device:"Device",sec_device_desc:"Language and factory reset.",pill_device:"Device",lang:"Language",
factory:"Factory Reset",boot_hint:"Or hold BOOT for 3 seconds while the device is already running.",
wifi_on:"Connected",wifi_off:"Not connected",wifi_try:"Connecting…",wifi_fail:"Wi-Fi failed",
mqtt_on:"MQTT connected",mqtt_off:"Not connected",mqtt_wait:"Waiting for printer details",
ap_on:"Active",ap_off:"Off",saved:"Saved",rebooting:"Rebooting… reload the page afterwards.",
ota_ok:"Update complete, rebooting…",ota_fail:"Update failed",
factory_ask:"Erase all settings and reboot?",
state_provisioning:"Not set up",state_wifi:"Joining Wi-Fi",state_mqtt:"Connecting printer",
state_idle:"Idle",state_homing:"Homing",state_leveling:"Leveling",state_preparing:"Preparing",
state_downloading:"Downloading",state_heating:"Heating",state_printing:"Printing",state_paused:"Paused",
state_finished:"Finished",state_error:"Error",state_standby:"Standby",state_unknown:"—",
err_bad_ssid:"Invalid SSID",err_bad_pass:"Invalid password",err_bad_json:"Invalid input",
err_ap_pass:"Hotspot password: empty or ≥ 8 characters",err_bad_ip:"Invalid IP",
err_bad_host:"Hostname: letters, digits, hyphen or underscore",err_bad_printer:"Invalid printer details",
err_bad_leds:"Invalid light values"
}};
let lang="de", pollTimer=null, ledTimer=null, openedOnce=false;
const $=id=>document.getElementById(id);
const LIGHT_SLOTS=[
  {key:"idle",color:"#ffffff",fx:"pulse"},
  {key:"homing",color:"#ff7800",fx:"comet"},
  {key:"leveling",color:"#ff7800",fx:"comet"},
  {key:"preparing",color:"#ff7800",fx:"comet"},
  {key:"heating",color:"#ff5000",fx:"progress"},
  {key:"downloading",color:"#ffdc00",fx:"comet"},
  {key:"print",color:"#ffffff",fx:"progress"},
  {key:"paused",color:"#ffffff",fx:"pulse"},
  {key:"finished",color:"#00ff00",fx:"solid"},
  {key:"error",color:"#ff0000",fx:"strobe"}
];
const FX_LIST=["pulse","solid","comet","wave","strobe","progress","rainbow","sparkle","fire"];
function t(k){return (I18N[lang]&&I18N[lang][k])||I18N.de[k]||k}
function fxOptionsHtml(selected){
  return FX_LIST.map(v=>'<option value="'+v+'" data-i="fx_'+v+'"'+(v===selected?' selected':'')+'>'+t('fx_'+v)+'</option>').join('');
}
function buildLightRows(){
  const root=$("lightStates"); if(!root||root.dataset.ready) return;
  root.innerHTML=LIGHT_SLOTS.map(s=>{
    const colorId=s.key+'Color', fxId=s.key+'Effect';
    return '<div class="light-row">'+
      '<div class="field"><label for="'+colorId+'" data-i="light_'+s.key+'">'+t('light_'+s.key)+'</label>'+
      '<input id="'+colorId+'" type="color" value="'+s.color+'"></div>'+
      '<div class="field"><label for="'+fxId+'" data-i="light_'+s.key+'_fx">'+t('light_'+s.key+'_fx')+'</label>'+
      '<select id="'+fxId+'">'+fxOptionsHtml(s.fx)+'</select></div></div>';
  }).join('');
  root.dataset.ready='1';
  LIGHT_SLOTS.forEach(s=>{
    $(s.key+'Color').addEventListener('input',scheduleLeds);
    $(s.key+'Effect').addEventListener('change',scheduleLeds);
  });
}
function applyLang(){
  document.documentElement.lang=lang;
  document.querySelectorAll("[data-i]").forEach(el=>{el.textContent=t(el.dataset.i)});
  LIGHT_SLOTS.forEach(s=>{
    const sel=$(s.key+'Effect'); if(!sel) return;
    [...sel.options].forEach(o=>{ if(o.dataset.i) o.textContent=t(o.dataset.i); });
  });
}
function setStatus(line,detail){$("status").textContent=line||"";$("statusDetail").textContent=detail||""}
function errText(code){return t("err_"+String(code||"").replace(/-/g,"_"))}
async function api(path,body){
  const opt={method:"POST",headers:{"Content-Type":"application/json"}};
  if(body!==undefined) opt.body=JSON.stringify(body);
  const res=await fetch(path,opt);
  const data=await res.json().catch(()=>({}));
  if(!res.ok||data.ok===false) throw new Error(data.error||"bad-json");
  return data;
}
function setIdle(id,value,opts){
  const el=$(id); if(!el||document.activeElement===el) return;
  if(el.type==="checkbox"){ el.checked=!!value; return; }
  // Status polling must not wipe values the user just filled from a scan card
  // with empty NVS (printer not bound yet).
  if(opts&&opts.keepIfEmpty&&!value&&el.value) return;
  el.value=value;
}
function setBadge(el,cls){el.className="badge "+cls}
function setPill(el,text,cls){el.textContent=text;el.className="summary-pill "+cls}
function stateLabel(data){
  let key="state_"+(data.state||"unknown");
  if(data.asleep&&(data.state==="idle"||data.state==="finished")) key="state_standby";
  let text=t(key);
  if((data.state==="printing"||data.state==="heating")&&typeof data.progress==="number") text+=" "+data.progress+"%";
  return text;
}
function applyStatus(data){
  if(data.language&&data.language!==lang){lang=data.language;applyLang()}
  const wifi=data.wifi||{}, ap=data.ap||{}, printer=data.printer||{}, leds=data.leds||{};
  $("heroVersion").textContent=data.version||"";
  const wifiOk=!!wifi.connected, mqttOk=!!printer.mqtt, printerReady=!!printer.ready;
  setBadge($("wifiBadge"), wifi.connecting?"warn":(wifiOk?"ok":(wifi.failed?"warn":"idle")));
  $("wifiBadgeVal").textContent=wifi.connecting?(t("wifi_try")+(wifi.target||"")):(wifiOk?(wifi.ssid||t("wifi_on")):t("wifi_off"));
  setBadge($("localBadge"), mqttOk?"ok":(printerReady?"warn":"idle"));
  $("localBadgeVal").textContent=mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait"));
  const st=stateLabel(data);
  setBadge($("stateBadge"), data.state==="error"?"warn":(wifiOk?"info":"idle"));
  $("stateBadgeVal").textContent=st;
  setBadge($("apBadge"), ap.running?"info":"idle");
  $("apBadgeVal").textContent=ap.running?((ap.ssid||t("ap_on"))):t("ap_off");
  $("heroHintText").textContent=wifiOk?t("hint_ok"):t("hint_ap");
  setPill($("wifiPill"), wifiOk?(wifi.ip||t("wifi_on")):t("wifi_off"), wifiOk?"ok":(wifi.failed?"warn":"idle"));
  setPill($("localPill"), mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait")), mqttOk?"ok":(printerReady?"warn":"idle"));
  setPill($("apPill"), ap.running?t("ap_on"):t("ap_off"), ap.running?"info":"idle");
  $("wifiDetail").textContent=wifi.connecting?(t("wifi_try")+(wifi.target||"")):(wifiOk?(wifi.ssid+" · "+wifi.ip):t("wifi_off"));
  $("mqttDetail").textContent=mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait"));
  setIdle("host", data.hostname||"");
  setIdle("ssid", wifi.ssid||"");
  setIdle("printerIp", printer.ip||"", {keepIfEmpty:true});
  setIdle("printerSerial", printer.serial||"", {keepIfEmpty:true});
  if(document.activeElement!==$("printerCode") && printer.code) $("printerCode").placeholder="••••••••";
  buildLightRows();
  if(document.activeElement!==$("brightness")) $("brightness").value=leds.brightness??50;
  if(document.activeElement!==$("speed")) $("speed").value=leds.speed??50;
  LIGHT_SLOTS.forEach(s=>{
    const def=s;
    setIdle(s.key+'Color', leds[s.key]||def.color);
    setIdle(s.key+'Effect', leds[s.key+'Effect']||def.fx);
  });
  setIdle("ledReverse", !!leds.reverse);
  $("briVal").textContent=$("brightness").value+"%";
  $("spdVal").textContent=$("speed").value+"%";
  setIdle("apEnabled", !!ap.enabled);
  setIdle("apSsid", ap.ssid||"");
  setIdle("apPass", ap.password||"");
  setIdle("apIp", ap.ip||"");
  setIdle("language", lang);
  let line=st;
  if(wifiOk) line+=" · "+wifi.ip; else if(ap.running) line+=" · "+ap.ip;
  setStatus(line, wifi.failed?t("wifi_fail"):"");
  if(!openedOnce){
    openedOnce=true;
    $("secWifi").open=!wifiOk;
    $("secPrinter").open=wifiOk&&!mqttOk;
    $("secLight").open=wifiOk&&mqttOk;
  }
}
async function refresh(){try{const res=await fetch("/api/status",{cache:"no-store"});applyStatus(await res.json())}catch(e){}}
function startPoll(){if(!pollTimer) pollTimer=setInterval(refresh,2000)}
async function pollList(startPath,listPath,render){
  await api(startPath,{});
  for(let i=0;i<40;i++){
    const res=await fetch(listPath,{cache:"no-store"});
    const data=await res.json();
    render(data);
    if(!data.scanning) return;
    await new Promise(r=>setTimeout(r,400));
  }
}
function renderWifi(data){
  const sel=$("wifiSelect");
  sel.innerHTML='<option value="">'+(lang==="de"?"Netzwerk wählen…":"Select detected Wi-Fi…")+'</option>';
  (data.networks||[]).forEach(n=>{
    const o=document.createElement("option");
    o.value=n.ssid; o.textContent=n.ssid+"  "+n.rssi+" dBm"+(n.secure?" *":"");
    sel.appendChild(o);
  });
  $("wifiSelectField").style.display=(data.networks&&data.networks.length)?"grid":"none";
  $("wifiScanDetail").textContent=data.scanning?(lang==="de"?"Suche…":"Scanning…"):t("scan_hint");
}
function renderPrinters(data){
  const box=$("printerList"); box.innerHTML="";
  (data.printers||[]).forEach(p=>{
    const b=document.createElement("button");
    b.type="button"; b.className="printer-card";
    b.innerHTML="<strong>"+(p.name||p.serial||"Bambu")+"</strong><span class='micro'>"+p.ip+(p.serial?" · "+p.serial:"")+"</span>";
    b.onclick=()=>{ $("printerIp").value=p.ip||""; if(p.serial) $("printerSerial").value=p.serial; };
    box.appendChild(b);
  });
  $("printerScanDetail").textContent=data.scanning?(lang==="de"?"Suche…":"Scanning…"):t("printer_scan_hint");
}
function scheduleLeds(){clearTimeout(ledTimer);ledTimer=setTimeout(saveLeds,250)}
async function saveLeds(){
  buildLightRows();
  const body={brightness:Number($("brightness").value),speed:Number($("speed").value),reverse:$("ledReverse").checked};
  LIGHT_SLOTS.forEach(s=>{ body[s.key]=$(s.key+'Color').value; body[s.key+'Effect']=$(s.key+'Effect').value; });
  try{await api("/api/leds",body);setStatus(t("saved"),"")}
  catch(e){setStatus(errText(e.message),"")}
}
$("wifiSelect").onchange=()=>{if($("wifiSelect").value) $("ssid").value=$("wifiSelect").value};
$("wifiScan").onclick=async()=>{
  $("wifiScan").disabled=true;
  try{await pollList("/api/wifi/scan","/api/wifi/scan",renderWifi)}catch(e){setStatus(errText(e.message),"")}
  $("wifiScan").disabled=false;
};
$("wifiBtn").onclick=async()=>{
  try{await api("/api/wifi",{ssid:$("ssid").value,password:$("wifiPass").value});setStatus(t("wifi_try")+$("ssid").value,"")}
  catch(e){setStatus(errText(e.message),"")}
};
$("hostBtn").onclick=async()=>{
  try{const d=await api("/api/hostname",{hostname:$("host").value.trim()});setStatus(d.reboot?t("rebooting"):t("saved"),"");if(d.reboot)clearInterval(pollTimer)}
  catch(e){setStatus(errText(e.message),"")}
};
$("printerScan").onclick=async()=>{
  $("printerScan").disabled=true;
  try{await pollList("/api/printer/scan","/api/printer/scan",renderPrinters)}catch(e){setStatus(errText(e.message),"")}
  $("printerScan").disabled=false;
};
$("bindBtn").onclick=async()=>{
  try{await api("/api/printer",{ip:$("printerIp").value.trim(),serial:$("printerSerial").value.trim(),code:$("printerCode").value.trim()});setStatus(t("saved"),"");$("printerCode").value=""}
  catch(e){setStatus(errText(e.message),"")}
};
["brightness","speed"].forEach(id=>{
  $(id).addEventListener("input",()=>{$("briVal").textContent=$("brightness").value+"%";$("spdVal").textContent=$("speed").value+"%";scheduleLeds()});
});
$("ledReverse").addEventListener("change",scheduleLeds);
$("lightReset").onclick=async()=>{try{await api("/api/leds/reset",{});setStatus(t("saved"),"");refresh()}catch(e){setStatus(errText(e.message),"")}};
buildLightRows();
$("apBtn").onclick=async()=>{
  try{const d=await api("/api/ap",{enabled:$("apEnabled").checked,ssid:$("apSsid").value.trim(),password:$("apPass").value,ip:$("apIp").value.trim()});setStatus(d.reboot?t("rebooting"):t("saved"),"");if(d.reboot)clearInterval(pollTimer)}
  catch(e){setStatus(errText(e.message),"")}
};
$("language").onchange=async()=>{lang=$("language").value;applyLang();try{await api("/api/language",{language:lang})}catch(e){setStatus(errText(e.message),"")}};
let otaDownloadUrl="";
function setOtaInfo(current,latest,available){
  const cur=current||"—", lat=latest||"—";
  $("otaVersionInfo").textContent=(lang==="de"?"Aktuell: ":"Current: ")+cur+" · "+(lang==="de"?"Neueste: ":"Latest: ")+lat;
  $("otaInstallBtn").disabled=!available;
  setPill($("otaPill"), available?(lang==="de"?"Update":"Update"):t("pill_ota"), available?"warn":"idle");
}
$("otaCheckBtn").onclick=async()=>{
  $("otaCheckBtn").disabled=true;
  $("otaGhStatus").textContent=t("ota_checking");
  try{
    const res=await fetch("/api/update/check",{cache:"no-store"});
    const data=await res.json();
    if(!data.ok){ $("otaGhStatus").textContent=errText(data.error||"bad-json"); setOtaInfo(data.current,"",false); }
    else{
      otaDownloadUrl=data.url||"";
      setOtaInfo(data.current,data.latest,!!data.available);
      $("otaGhStatus").textContent=data.available?t("ota_available"):t("ota_up_to_date");
    }
  }catch(e){ $("otaGhStatus").textContent=errText(e.message); }
  $("otaCheckBtn").disabled=false;
};
$("otaInstallBtn").onclick=async()=>{
  if(!otaDownloadUrl){ $("otaGhStatus").textContent=t("ota_gh_ready"); return; }
  $("otaInstallBtn").disabled=true; $("otaCheckBtn").disabled=true;
  $("otaGhStatus").textContent=t("ota_installing");
  $("otaProg").style.display="block"; $("otaBar").style.width="35%";
  try{
    await api("/api/update/github",{url:otaDownloadUrl});
    $("otaBar").style.width="100%";
    setStatus(t("ota_ok"),""); $("otaGhStatus").textContent=t("ota_ok");
    clearInterval(pollTimer);
  }catch(e){
    $("otaBar").style.width="0%";
    setStatus(t("ota_fail"),""); $("otaGhStatus").textContent=errText(e.message);
    $("otaInstallBtn").disabled=false; $("otaCheckBtn").disabled=false;
  }
};
$("otaBtn").onclick=()=>{
  const file=$("fw").files&&$("fw").files[0]; if(!file) return;
  const xhr=new XMLHttpRequest(), body=new FormData();
  body.append("firmware",file,file.name);
  $("otaProg").style.display="block"; $("otaBar").style.width="0%"; $("otaStatus").textContent=lang==="de"?"Lade hoch…":"Uploading…";
  xhr.open("POST","/api/update");
  xhr.upload.onprogress=ev=>{if(ev.lengthComputable)$("otaBar").style.width=Math.round(ev.loaded*100/ev.total)+"%"};
  xhr.onload=()=>{setStatus(xhr.status===200?t("ota_ok"):t("ota_fail"),"");$("otaStatus").textContent=xhr.status===200?t("ota_ok"):t("ota_fail");clearInterval(pollTimer)};
  xhr.onerror=()=>{setStatus(t("ota_fail"),"");$("otaStatus").textContent=t("ota_fail")};
  xhr.send(body);
};
$("resetBtn").onclick=async()=>{
  if(!confirm(t("factory_ask"))) return;
  try{await api("/api/factory-reset",{});setStatus(t("rebooting"),"");clearInterval(pollTimer)}catch(e){setStatus(errText(e.message),"")}
};
applyLang(); refresh(); startPoll();
</script>
</body>
</html>
)PAGE";

// ==== WebUi.h ====
// Local web UI: setup pages plus file and GitHub OTA.
// The AP stays up whenever Wi-Fi is down so a bad password cannot lock the device out.
class WebUi {
 public:
  void begin();
  void loop();

  void setLive(const char* state, uint8_t progress, bool asleep, bool mqttConnected);
  void onOtaStart(std::function<void()> cb) { onOtaStart_ = std::move(cb); }

  void factoryReset();
  bool consumeActivity();
  bool staConnected() const { return staConnected_; }
  bool printerReady() const { return store_.printerReady(); }
  bool otaBusy() const { return otaBusy_; }
  uint32_t revision() const { return store_.revision(); }
  const DeviceSettings& settings() const { return store_.data(); }

 private:
  struct FoundNet {
    String ssid;
    int32_t rssi;
    bool secure;
  };
  struct FoundPrinter {
    String ip;
    String serial;
    String name;
  };

  void applyHostname();
  void pushHostnameToDhcp();
  String dhcpHostname() const;
  void startAp();
  void stopAp();
  void syncAp(bool staUp);
  void startMdns();
  void pollWifiConnect();
  void pollWifiScan();
  void pollPrinterScan();
  void notePrinterDatagram(const uint8_t* data, int len);

  void noteActivity() { activity_ = true; }
  void sendJson(int code, const String& body);
  void sendError(int code, const char* key);
  void sendOk(bool reboot);
  bool readJson(ArduinoJson::JsonDocument& doc);
  void sendPage();

  void handleStatus();
  void handleWifiScanGet();
  void handleWifiScanPost();
  void handleWifiConnect();
  void handleHostname();
  void handlePrinterScanGet();
  void handlePrinterScanPost();
  void handlePrinterSave();
  void handleLeds();
  void handleLedsReset();
  void handleAp();
  void handleLanguage();
  void handleFactory();
  void handleUpdateCheck();
  void handleUpdateGithub();
  void handleNotFound();

  SettingsStore store_;
  bool staConnected_ = false;
  bool apRunning_ = false;
  int apChannel_ = 1;
  bool mdnsStarted_ = false;

  bool channelAligned_ = false;
  bool connectArmed_ = false;
  bool connectPending_ = false;
  bool connectFailed_ = false;
  bool connectSawDrop_ = false;
  unsigned long connectArmAt_ = 0;
  unsigned long connectStarted_ = 0;
  String pendingSsid_;
  String pendingPass_;

  bool wifiScanRunning_ = false;
  bool printerScanRunning_ = false;
  unsigned long printerScanDeadline_ = 0;
  bool hostnamePushed_ = false;
  unsigned long staLostSinceMs_ = 0;

  bool activity_ = false;
  bool otaBusy_ = false;
  bool otaReject_ = false;
  std::function<void()> onOtaStart_;

  const char* liveState_ = "provisioning";
  uint8_t liveProgress_ = 0;
  bool liveAsleep_ = false;
  bool liveMqtt_ = false;

  // Containers live in the .cpp so this header stays free of WebServer/UDP includes.
  struct Impl;
  Impl* impl_ = nullptr;
};

// ==== WebUi.cpp ====
struct WebUi::Impl {
  WebServer server{80};
  DNSServer dns;
  WiFiUDP udp;
  std::vector<FoundNet> networks;
  std::vector<FoundPrinter> printers;
};

namespace {
bool parseIpv4(const String& text, IPAddress& out) {
  String s = text;
  s.trim();
  int parts[4] = {0, 0, 0, 0};
  int idx = 0;
  int start = 0;
  for (int i = 0; i <= s.length() && idx < 4; i++) {
    if (i != s.length() && s[i] != '.') continue;
    if (i == start) return false;
    String piece = s.substring(start, i);
    if (piece.length() == 0 || piece.length() > 3) return false;
    for (unsigned j = 0; j < piece.length(); j++) {
      if (!isdigit(static_cast<unsigned char>(piece[j]))) return false;
    }
    int value = piece.toInt();
    if (value < 0 || value > 255) return false;
    parts[idx++] = value;
    start = i + 1;
  }
  if (idx != 4 || start != s.length() + 1) return false;
  out = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return true;
}

bool extractIpv4(const String& text, String& ip) {
  for (int i = 0; i < text.length(); i++) {
    if (!isdigit(static_cast<unsigned char>(text[i]))) continue;
    int end = i;
    while (end < text.length() &&
           (isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) {
      end++;
    }
    String token = text.substring(i, end);
    IPAddress addr;
    if (parseIpv4(token, addr) && addr[0] != 0 && addr[0] != 239 && addr[0] != 255) {
      ip = addr.toString();
      return true;
    }
    if (end == i) end++;
    i = end;
  }
  return false;
}

bool validHostname(const char* text) {
  if (!text) return false;
  size_t n = strlen(text);
  // DHCP/mDNS label; keep room for the trailing NUL in esp_netif (32 bytes).
  if (n < 1 || n > 31) return false;
  if (text[0] == '-' || text[0] == '_' || text[n - 1] == '-' || text[n - 1] == '_') return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    // Underscores are not classic DNS labels, but Fritzbox and many routers
    // accept them in the DHCP client hostname — users expect names like
    // Panda_Status_A1_Mini.
    if (!isalnum(c) && c != '-' && c != '_') return false;
  }
  return true;
}

bool validSsid(const char* text) {
  if (!text) return false;
  size_t n = strlen(text);
  if (n < 1 || n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 32 || c == 127) return false;
  }
  return true;
}

bool validSecret(const char* text, bool allowEmpty) {
  if (!text) return false;
  size_t n = strlen(text);
  if (n == 0) return allowEmpty;
  if (n > 64) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 32 || c == 127) return false;
  }
  return true;
}

bool validToken(const char* text, size_t minLen, size_t maxLen) {
  if (!text) return false;
  size_t n = strlen(text);
  if (n < minLen || n > maxLen) return false;
  for (size_t i = 0; i < n; i++) {
    if (!isalnum(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

bool validHostOrIp(const char* text) {
  if (!text) return false;
  size_t n = strlen(text);
  if (n < 1 || n > 40) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
  }
  return true;
}

String colorHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

bool parseColor(const char* text, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!text || strlen(text) != 7 || text[0] != '#') return false;
  unsigned int value = 0;
  if (sscanf(text + 1, "%06x", &value) != 1) return false;
  r = (value >> 16) & 0xFF;
  g = (value >> 8) & 0xFF;
  b = value & 0xFF;
  return true;
}

String headerValue(const String& text, const char* key) {
  String lower = text;
  lower.toLowerCase();
  String needle(key);
  needle.toLowerCase();
  needle += ":";
  int at = 0;
  while (at >= 0 && at < lower.length()) {
    int found = lower.indexOf(needle, at);
    if (found < 0) return "";
    // Match only at the start of a line so "DevName.bambu.com" does not hit
    // a substring of another header.
    if (found == 0 || lower[found - 1] == '\n') {
      int colon = found + needle.length() - 1;
      int end = text.indexOf('\n', colon);
      if (end < 0) end = text.length();
      String value = text.substring(colon + 1, end);
      value.trim();
      value.replace("\r", "");
      return value;
    }
    at = found + 1;
  }
  return "";
}
}  // namespace

void WebUi::begin() {
  static Impl storage;
  impl_ = &storage;
  store_.begin();

  if (store_.data().apSsid.isEmpty()) {
    store_.data().apSsid = "BambuStatusBar";
    store_.save();
  }

  WiFi.persistent(false);
  WiFi.setSleep(false);
  // DHCP picks up NetworkManager's default hostname when STA is created.
  // Set it before WiFi.mode(), otherwise the Fritzbox keeps esp32c6-XXXXXX.
  applyHostname();
  WiFi.mode(WIFI_AP_STA);
  applyHostname();
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);

  startAp();
  if (store_.data().wifiSsid.length()) {
    applyHostname();
    WiFi.begin(store_.data().wifiSsid.c_str(), store_.data().wifiPass.c_str());
  }

  impl_->server.on("/", HTTP_GET, [this]() { sendPage(); });
  impl_->server.on("/index.html", HTTP_GET, [this]() { sendPage(); });
  impl_->server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  impl_->server.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScanGet(); });
  impl_->server.on("/api/wifi/scan", HTTP_POST, [this]() { handleWifiScanPost(); });
  impl_->server.on("/api/wifi", HTTP_POST, [this]() { handleWifiConnect(); });
  impl_->server.on("/api/hostname", HTTP_POST, [this]() { handleHostname(); });
  impl_->server.on("/api/printer/scan", HTTP_GET, [this]() { handlePrinterScanGet(); });
  impl_->server.on("/api/printer/scan", HTTP_POST, [this]() { handlePrinterScanPost(); });
  impl_->server.on("/api/printer", HTTP_POST, [this]() { handlePrinterSave(); });
  impl_->server.on("/api/leds", HTTP_POST, [this]() { handleLeds(); });
  impl_->server.on("/api/leds/reset", HTTP_POST, [this]() { handleLedsReset(); });
  impl_->server.on("/api/ap", HTTP_POST, [this]() { handleAp(); });
  impl_->server.on("/api/language", HTTP_POST, [this]() { handleLanguage(); });
  impl_->server.on("/api/factory-reset", HTTP_POST, [this]() { handleFactory(); });
  impl_->server.on("/api/update/check", HTTP_GET, [this]() { handleUpdateCheck(); });
  impl_->server.on("/api/update/github", HTTP_POST, [this]() { handleUpdateGithub(); });
  impl_->server.on(
      "/api/update", HTTP_POST,
      [this]() {
        noteActivity();
        if (Update.hasError() || otaReject_) {
          otaBusy_ = false;
          otaReject_ = false;
          impl_->server.send(500, "text/plain", "FAIL");
          return;
        }
        impl_->server.send(200, "text/plain", "OK");
        delay(300);
        ESP.restart();
      },
      [this]() {
        HTTPUpload& upload = impl_->server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          noteActivity();
          otaBusy_ = true;
          otaReject_ = false;
          String name = upload.filename;
          name.toLowerCase();
          if (name.length() && !name.endsWith(".bin")) {
            otaReject_ = true;
            return;
          }
          if (onOtaStart_) onOtaStart_();
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaReject_ = true;
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (otaReject_) return;
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) otaReject_ = true;
        } else if (upload.status == UPLOAD_FILE_END) {
          if (otaReject_ || !Update.end(true)) {
            Update.abort();
            otaReject_ = true;
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          Update.abort();
          otaReject_ = true;
          otaBusy_ = false;
        }
      });
  impl_->server.onNotFound([this]() { handleNotFound(); });
  impl_->server.begin();

  Serial.print(F("AP SSID "));
  Serial.println(store_.data().apSsid);
  Serial.print(F("AP password "));
  Serial.println(store_.data().apPass);
  Serial.print(F("AP IP http://"));
  Serial.println(WiFi.softAPIP());
}

void WebUi::loop() {
  if (!impl_) return;
  if (apRunning_) impl_->dns.processNextRequest();
  impl_->server.handleClient();
  pollWifiScan();
  pollPrinterScan();
  pollWifiConnect();
  if (staConnected_ && !mdnsStarted_) startMdns();
  if (!staConnected_) mdnsStarted_ = false;
  syncAp(staConnected_);
}

void WebUi::setLive(const char* state, uint8_t progress, bool asleep, bool mqttConnected) {
  liveState_ = state ? state : "unknown";
  liveProgress_ = progress;
  liveAsleep_ = asleep;
  liveMqtt_ = mqttConnected;
}

void WebUi::factoryReset() {
  store_.clear();
  WiFi.disconnect(true, true);
  delay(150);
  ESP.restart();
}

bool WebUi::consumeActivity() {
  bool seen = activity_;
  activity_ = false;
  return seen;
}

String WebUi::dhcpHostname() const {
  // Fritzbox (and many routers) ignore or rewrite DHCP hostnames that contain
  // underscores. Keep the stored name readable, but put a DNS/DHCP-safe form
  // on the wire (underscore -> hyphen).
  String wire = store_.data().hostname;
  if (wire.isEmpty()) wire = "BambuStatusBar";
  wire.replace('_', '-');
  if (wire.length() > 31) wire = wire.substring(0, 31);
  while (wire.endsWith("-")) wire.remove(wire.length() - 1);
  if (wire.isEmpty()) wire = "BambuStatusBar";
  return wire;
}

void WebUi::applyHostname() {
  const String wire = dhcpHostname();
  WiFi.setHostname(wire.c_str());
  // Apply to live interfaces too (no-op if netif not created yet).
  const bool staOk = WiFi.STA.setHostname(wire.c_str());
  WiFi.softAPsetHostname(wire.c_str());
  Serial.print(F("Hostname stored="));
  Serial.print(store_.data().hostname);
  Serial.print(F(" dhcp="));
  Serial.print(wire);
  Serial.print(F(" sta="));
  Serial.println(staOk ? F("ok") : F("pending"));
}

void WebUi::pushHostnameToDhcp() {
  // Only refresh the hostname on the STA netif. Do NOT call lwIP
  // dhcp_release_and_stop/dhcp_start from the Arduino loop — that skips the
  // TCPIP core lock and can panic into a boot loop once STA is up.
  const String wire = dhcpHostname();
  applyHostname();
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) {
    esp_err_t err = esp_netif_set_hostname(netif, wire.c_str());
    Serial.print(F("DHCP hostname set "));
    Serial.print(wire);
    Serial.print(F(" err="));
    Serial.println(static_cast<int>(err));
  } else {
    Serial.println(F("DHCP hostname: STA netif missing"));
  }
}

void WebUi::startAp() {
  applyHostname();
  IPAddress ip;
  if (!parseIpv4(store_.data().apIp, ip) || ip[0] == 0) ip = IPAddress(192, 168, 254, 1);
  int channel = 1;
  if (WiFi.status() == WL_CONNECTED) {
    int staChannel = WiFi.channel();
    if (staChannel > 0 && staChannel <= 13) channel = staChannel;
  }
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    applyHostname();
  }
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  const String& ssid = store_.data().apSsid;
  const String& pass = store_.data().apPass;
  bool ok = pass.length() >= 8 ? WiFi.softAP(ssid.c_str(), pass.c_str(), channel)
                               : WiFi.softAP(ssid.c_str(), nullptr, channel);
  if (!ok) {
    Serial.println(F("softAP failed"));
    apRunning_ = false;
    return;
  }
  apChannel_ = channel;
  impl_->dns.stop();
  impl_->dns.start(53, "*", WiFi.softAPIP());
  apRunning_ = true;
}

void WebUi::stopAp() {
  impl_->dns.stop();
  WiFi.softAPdisconnect(true);
  apRunning_ = false;
}

void WebUi::syncAp(bool staUp) {
  bool want = store_.data().apEnabled || !staUp;
  if (want && apRunning_ && staUp && !channelAligned_) {
    int channel = WiFi.channel();
    if (channel > 0 && channel != apChannel_) startAp();
    channelAligned_ = true;
  }
  if (!staUp) channelAligned_ = false;
  if (want == apRunning_) return;
  if (want) startAp();
  else stopAp();
}

void WebUi::startMdns() {
  if (!MDNS.begin(store_.data().hostname.c_str())) return;
  MDNS.addService("http", "tcp", 80);
  mdnsStarted_ = true;
  Serial.print(F("mDNS http://"));
  Serial.print(store_.data().hostname);
  Serial.println(F(".local"));
}

void WebUi::pollWifiConnect() {
  // Arm the join only after the HTTP response has been flushed. A success
  // counts only after the link actually drops, otherwise a still-connected
  // old network would be stored as the new credentials.
  if (connectArmed_ && static_cast<long>(millis() - connectArmAt_) >= 0) {
    connectArmed_ = false;
    connectPending_ = true;
    connectFailed_ = false;
    connectSawDrop_ = false;
    connectStarted_ = millis();
    WiFi.disconnect(false, false);
    applyHostname();
    WiFi.begin(pendingSsid_.c_str(), pendingPass_.c_str());
  }
  bool up = WiFi.status() == WL_CONNECTED;
  if (connectPending_) {
    if (!up) connectSawDrop_ = true;
    if (up && connectSawDrop_ && WiFi.SSID() == pendingSsid_) {
      connectPending_ = false;
      connectFailed_ = false;
      store_.data().wifiSsid = pendingSsid_;
      store_.data().wifiPass = pendingPass_;
      store_.save();
      Serial.print(F("WiFi connected "));
      Serial.println(WiFi.localIP());
      if (!hostnamePushed_) {
        hostnamePushed_ = true;
        pushHostnameToDhcp();
      }
    } else if (millis() - connectStarted_ > 20000) {
      connectPending_ = false;
      connectFailed_ = true;
      Serial.println(F("WiFi connect failed"));
      if (store_.data().wifiSsid.length()) {
        applyHostname();
        WiFi.begin(store_.data().wifiSsid.c_str(), store_.data().wifiPass.c_str());
      }
    }
  }
  staConnected_ = WiFi.status() == WL_CONNECTED;
  if (staConnected_) {
    staLostSinceMs_ = 0;
    if (!hostnamePushed_ && !connectPending_ && !connectArmed_) {
      hostnamePushed_ = true;
      pushHostnameToDhcp();
    }
  } else if (staLostSinceMs_ == 0) {
    staLostSinceMs_ = millis();
  } else if (hostnamePushed_ && millis() - staLostSinceMs_ > 5000) {
    hostnamePushed_ = false;
  }
}

void WebUi::pollWifiScan() {
  if (!wifiScanRunning_) return;
  int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) return;
  impl_->networks.clear();
  if (count > 0) {
    for (int i = 0; i < count && impl_->networks.size() < 20; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      int32_t rssi = WiFi.RSSI(i);
      bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      bool merged = false;
      for (auto& existing : impl_->networks) {
        if (existing.ssid == ssid) {
          merged = true;
          if (rssi > existing.rssi) existing.rssi = rssi;
          break;
        }
      }
      if (!merged) impl_->networks.push_back(FoundNet{ssid, rssi, secure});
    }
  }
  WiFi.scanDelete();
  wifiScanRunning_ = false;
}

void WebUi::pollPrinterScan() {
  if (!printerScanRunning_) return;
  for (int i = 0; i < 8; i++) {
    int packet = impl_->udp.parsePacket();
    if (packet <= 0) break;
    uint8_t buf[1024];
    int len = impl_->udp.read(buf, sizeof(buf) - 1);
    if (len < 0) len = 0;
    buf[len] = 0;
    notePrinterDatagram(buf, len);
  }
  if (static_cast<long>(millis() - printerScanDeadline_) >= 0) {
    printerScanRunning_ = false;
    impl_->udp.stop();
  }
}

void WebUi::notePrinterDatagram(const uint8_t* data, int len) {
  // Bambu printers announce themselves with SSDP-style NOTIFY packets on
  // UDP broadcast port 2021 (about every 5 s). They do not answer classic
  // M-SEARCH on 1900 the way stock UPnP devices do.
  String text(reinterpret_cast<const char*>(data), len);
  String lower = text;
  lower.toLowerCase();
  if (lower.indexOf("bambulab") < 0 && lower.indexOf("3dprinter") < 0 &&
      lower.indexOf("devmodel.bambu.com") < 0) {
    return;
  }

  String ip = headerValue(text, "location");
  ip.trim();
  IPAddress parsed;
  if (!parseIpv4(ip, parsed)) {
    ip = "";
    extractIpv4(text, ip);
  }
  if (ip.isEmpty()) {
    IPAddress remote = impl_->udp.remoteIP();
    if (remote[0] != 0) ip = remote.toString();
  }
  if (ip.isEmpty()) return;

  String serial = headerValue(text, "usn");
  int sep = serial.indexOf("::");
  if (sep >= 0) serial = serial.substring(0, sep);
  serial.replace("uuid:", "");
  serial.trim();
  // Bambu USN is the bare printer serial (alphanumeric). Reject UUID-like junk.
  if (serial.indexOf('-') >= 0 || serial.indexOf(':') >= 0 || serial.indexOf('.') >= 0 ||
      serial.length() < 8 || serial.length() > 32) {
    serial = "";
  }

  String name = headerValue(text, "DevName.bambu.com");
  if (name.isEmpty()) name = headerValue(text, "devname.bambu.com");
  if (name.isEmpty()) name = headerValue(text, "devname");

  for (auto& existing : impl_->printers) {
    if (existing.ip == ip || (!serial.isEmpty() && existing.serial == serial)) {
      if (existing.ip.isEmpty()) existing.ip = ip;
      if (existing.serial.isEmpty()) existing.serial = serial;
      if (existing.name.isEmpty()) existing.name = name;
      return;
    }
  }
  if (impl_->printers.size() < 8) impl_->printers.push_back(FoundPrinter{ip, serial, name});
}

void WebUi::sendJson(int code, const String& body) {
  impl_->server.send(code, "application/json", body);
}

void WebUi::sendError(int code, const char* key) {
  noteActivity();
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = key;
  String body;
  serializeJson(doc, body);
  sendJson(code, body);
}

void WebUi::sendOk(bool reboot) {
  noteActivity();
  sendJson(200, reboot ? "{\"ok\":true,\"reboot\":true}" : "{\"ok\":true}");
}

bool WebUi::readJson(JsonDocument& doc) {
  const String& body = impl_->server.arg("plain");
  if (body.length() > 1024 || deserializeJson(doc, body)) {
    sendError(400, "bad-json");
    return false;
  }
  return true;
}

void WebUi::sendPage() {
  noteActivity();
  impl_->server.sendHeader("Cache-Control", "no-store");
  impl_->server.send_P(200, "text/html", INDEX_HTML);
}

void WebUi::handleStatus() {
  noteActivity();
  const DeviceSettings& cfg = store_.data();
  JsonDocument doc;
  doc["ok"] = true;
  doc["version"] = kFirmwareVersion;
  doc["language"] = cfg.language;
  doc["hostname"] = cfg.hostname;
  doc["hostname_dhcp"] = dhcpHostname();
  {
    const char* live = nullptr;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_hostname(netif, &live) == ESP_OK && live) {
      doc["hostname_live"] = live;
    } else {
      doc["hostname_live"] = "";
    }
  }

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = staConnected_;
  wifi["ssid"] = cfg.wifiSsid;
  wifi["ip"] = staConnected_ ? WiFi.localIP().toString() : "";
  wifi["connecting"] = connectPending_ || connectArmed_;
  wifi["failed"] = connectFailed_;
  wifi["target"] = (connectPending_ || connectArmed_) ? pendingSsid_ : "";

  JsonObject ap = doc["ap"].to<JsonObject>();
  ap["enabled"] = cfg.apEnabled;
  ap["running"] = apRunning_;
  ap["ssid"] = cfg.apSsid;
  ap["password"] = cfg.apPass;
  ap["ip"] = apRunning_ ? WiFi.softAPIP().toString() : cfg.apIp;

  JsonObject printer = doc["printer"].to<JsonObject>();
  printer["ip"] = cfg.printerIp;
  printer["serial"] = cfg.printerSerial;
  printer["code"] = cfg.accessCode;
  printer["mqtt"] = liveMqtt_;
  printer["ready"] = store_.printerReady();

  JsonObject leds = doc["leds"].to<JsonObject>();
  auto putSlot = [&](const char* colorKey, const char* fxKey, const LightSlot& slot) {
    leds[colorKey] = colorHex(slot.r, slot.g, slot.b);
    leds[fxKey] = ledEffectName(slot.effect);
  };
  leds["brightness"] = cfg.brightness;
  leds["speed"] = cfg.speed;
  putSlot("idle", "idleEffect", cfg.idle);
  putSlot("print", "printEffect", cfg.printing);
  putSlot("error", "errorEffect", cfg.error);
  putSlot("homing", "homingEffect", cfg.homing);
  putSlot("leveling", "levelingEffect", cfg.leveling);
  putSlot("preparing", "preparingEffect", cfg.preparing);
  putSlot("heating", "heatingEffect", cfg.heating);
  putSlot("downloading", "downloadingEffect", cfg.downloading);
  putSlot("paused", "pausedEffect", cfg.paused);
  putSlot("finished", "finishedEffect", cfg.finished);
  leds["reverse"] = cfg.ledReverse;

  doc["state"] = liveState_;
  doc["progress"] = liveProgress_;
  doc["asleep"] = liveAsleep_;

  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebUi::handleWifiScanGet() {
  noteActivity();
  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = wifiScanRunning_;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (const auto& net : impl_->networks) {
    JsonObject item = arr.add<JsonObject>();
    item["ssid"] = net.ssid;
    item["rssi"] = net.rssi;
    item["secure"] = net.secure;
  }
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebUi::handleWifiScanPost() {
  noteActivity();
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING || wifiScanRunning_) {
    wifiScanRunning_ = true;
    sendOk(false);
    return;
  }
  impl_->networks.clear();
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  wifiScanRunning_ = true;
  sendOk(false);
}

void WebUi::handleWifiConnect() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["password"] | "";
  if (!validSsid(ssid)) {
    sendError(400, "bad-ssid");
    return;
  }
  if (!validSecret(pass, true)) {
    sendError(400, "bad-pass");
    return;
  }
  pendingSsid_ = ssid;
  pendingPass_ = pass;
  connectFailed_ = false;
  connectArmed_ = true;
  connectArmAt_ = millis() + 400;
  sendOk(false);
}

void WebUi::handleHostname() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* hostname = doc["hostname"] | "";
  if (!validHostname(hostname)) {
    sendError(400, "bad-host");
    return;
  }
  // Persist the DHCP-safe form so the UI and Fritzbox show the same name.
  String wire = hostname;
  wire.replace('_', '-');
  store_.data().hostname = wire;
  store_.save();
  sendOk(true);
  delay(250);
  ESP.restart();
}

void WebUi::handlePrinterScanGet() {
  noteActivity();
  JsonDocument doc;
  doc["ok"] = true;
  doc["scanning"] = printerScanRunning_;
  JsonArray arr = doc["printers"].to<JsonArray>();
  for (const auto& printer : impl_->printers) {
    JsonObject item = arr.add<JsonObject>();
    item["ip"] = printer.ip;
    item["serial"] = printer.serial;
    item["name"] = printer.name;
  }
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebUi::handlePrinterScanPost() {
  noteActivity();
  if (!staConnected_) {
    sendError(409, "no-wifi");
    return;
  }
  if (!printerScanRunning_) {
    impl_->printers.clear();
    impl_->udp.stop();
    // Bind on the STA address so AP+STA mode still receives LAN broadcasts.
    // Bambu printers broadcast NOTIFY to 255.255.255.255:2021 roughly every 5 s.
    bool bound = impl_->udp.begin(WiFi.localIP(), 2021);
    if (!bound) bound = impl_->udp.begin(2021);
    if (!bound) {
      sendError(500, "bad-json");
      return;
    }
    printerScanRunning_ = true;
    printerScanDeadline_ = millis() + 12000;
  }
  sendOk(false);
}

void WebUi::handlePrinterSave() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* ip = doc["ip"] | "";
  const char* serial = doc["serial"] | "";
  const char* code = doc["code"] | "";
  if (!validHostOrIp(ip) || !validToken(serial, 4, 32) || !validToken(code, 4, 16)) {
    sendError(400, "bad-printer");
    return;
  }
  store_.data().printerIp = ip;
  store_.data().printerSerial = serial;
  store_.data().accessCode = code;
  store_.save();
  sendOk(false);
}

void WebUi::handleLeds() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  if (!doc["brightness"].is<int>() || !doc["speed"].is<int>() || !doc["reverse"].is<bool>()) {
    sendError(400, "bad-leds");
    return;
  }
  int brightness = doc["brightness"].as<int>();
  int speed = doc["speed"].as<int>();
  if (brightness < 0 || brightness > 100 || speed < 1 || speed > 100) {
    sendError(400, "bad-leds");
    return;
  }

  auto readSlot = [&](const char* colorKey, const char* fxKey, LedEffect defFx, LightSlot& out) -> bool {
    uint8_t r, g, b;
    LedEffect fx = defFx;
    if (!parseColor(doc[colorKey] | "", r, g, b)) return false;
    if (!parseLedEffect(doc[fxKey] | ledEffectName(defFx), fx)) return false;
    out.r = r;
    out.g = g;
    out.b = b;
    out.effect = fx;
    return true;
  };

  DeviceSettings& cfg = store_.data();
  LightSlot idle = cfg.idle, printing = cfg.printing, error = cfg.error;
  LightSlot homing = cfg.homing, leveling = cfg.leveling, preparing = cfg.preparing;
  LightSlot heating = cfg.heating, downloading = cfg.downloading, paused = cfg.paused;
  LightSlot finished = cfg.finished;
  if (!readSlot("idle", "idleEffect", LedEffect::Pulse, idle) ||
      !readSlot("print", "printEffect", LedEffect::Progress, printing) ||
      !readSlot("error", "errorEffect", LedEffect::Strobe, error) ||
      !readSlot("homing", "homingEffect", LedEffect::Comet, homing) ||
      !readSlot("leveling", "levelingEffect", LedEffect::Comet, leveling) ||
      !readSlot("preparing", "preparingEffect", LedEffect::Comet, preparing) ||
      !readSlot("heating", "heatingEffect", LedEffect::Progress, heating) ||
      !readSlot("downloading", "downloadingEffect", LedEffect::Comet, downloading) ||
      !readSlot("paused", "pausedEffect", LedEffect::Pulse, paused) ||
      !readSlot("finished", "finishedEffect", LedEffect::Solid, finished)) {
    sendError(400, "bad-leds");
    return;
  }

  cfg.brightness = static_cast<uint8_t>(brightness);
  cfg.speed = static_cast<uint8_t>(speed);
  cfg.idle = idle;
  cfg.printing = printing;
  cfg.error = error;
  cfg.homing = homing;
  cfg.leveling = leveling;
  cfg.preparing = preparing;
  cfg.heating = heating;
  cfg.downloading = downloading;
  cfg.paused = paused;
  cfg.finished = finished;
  cfg.ledReverse = doc["reverse"].as<bool>();
  store_.save();
  sendOk(false);
}

void WebUi::handleLedsReset() {
  noteActivity();
  store_.resetLights();
  sendOk(false);
}

void WebUi::handleAp() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  if (!doc["enabled"].is<bool>()) {
    sendError(400, "bad-json");
    return;
  }
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["password"] | "";
  const char* ipText = doc["ip"] | "";
  IPAddress ip;
  if (!validSsid(ssid)) {
    sendError(400, "bad-ssid");
    return;
  }
  if (!(validSecret(pass, true) && (strlen(pass) == 0 || strlen(pass) >= 8))) {
    sendError(400, "ap-pass");
    return;
  }
  if (!parseIpv4(ipText, ip) || ip[0] == 0 || ip[3] == 255) {
    sendError(400, "bad-ip");
    return;
  }
  DeviceSettings& cfg = store_.data();
  cfg.apEnabled = doc["enabled"].as<bool>();
  cfg.apSsid = ssid;
  cfg.apPass = pass;
  cfg.apIp = ip.toString();
  store_.save();
  sendOk(true);
  delay(250);
  ESP.restart();
}

void WebUi::handleLanguage() {
  noteActivity();
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* language = doc["language"] | "";
  if (strcmp(language, "de") != 0 && strcmp(language, "en") != 0) {
    sendError(400, "bad-json");
    return;
  }
  store_.data().language = language;
  store_.save();
  sendOk(false);
}

void WebUi::handleFactory() {
  sendOk(true);
  delay(200);
  factoryReset();
}

void WebUi::handleUpdateCheck() {
  noteActivity();
  const GithubReleaseInfo info = GithubOta::checkLatest();
  JsonDocument doc;
  doc["ok"] = info.ok;
  doc["current"] = info.currentVersion;
  doc["latest"] = info.latestVersion;
  doc["available"] = info.updateAvailable;
  doc["url"] = info.downloadUrl;
  if (!info.error.isEmpty()) doc["error"] = info.error;
  String body;
  serializeJson(doc, body);
  sendJson(info.ok || info.error == "no-wifi" || info.error == "no-release" ? 200 : 200, body);
}

void WebUi::handleUpdateGithub() {
  noteActivity();
  if (otaBusy_) {
    sendError(409, "bad-json");
    return;
  }
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* url = doc["url"] | "";
  if (!url[0] || strncmp(url, "https://", 8) != 0) {
    sendError(400, "bad-json");
    return;
  }
  // Only allow GitHub download hosts.
  String urlStr = url;
  if (urlStr.indexOf("github.com/") < 0 && urlStr.indexOf("githubusercontent.com/") < 0) {
    sendError(400, "bad-json");
    return;
  }

  otaBusy_ = true;
  if (onOtaStart_) onOtaStart_();
  String error;
  const bool ok = GithubOta::installFromUrl(urlStr, error);
  if (!ok) {
    otaBusy_ = false;
    sendError(500, error.c_str());
    return;
  }
  sendOk(true);
  delay(400);
  ESP.restart();
}

void WebUi::handleNotFound() {
  String uri = impl_->server.uri();
  if (uri.startsWith("/api/")) {
    sendError(404, "bad-json");
    return;
  }
  if (!apRunning_) {
    impl_->server.send(404, "text/plain", "not found");
    return;
  }
  String host = impl_->server.hostHeader();
  host.toLowerCase();
  String apIp = WiFi.softAPIP().toString();
  String staIp = WiFi.localIP().toString();
  String name = store_.data().hostname;
  name.toLowerCase();
  bool ours = host.startsWith(apIp) || (staIp != "0.0.0.0" && host.startsWith(staIp)) || host.startsWith(name);
  if (ours) {
    impl_->server.send(404, "text/plain", "not found");
    return;
  }
  impl_->server.sendHeader("Location", "http://" + apIp + "/", true);
  impl_->server.send(302, "text/plain", "");
}

// ==== main.cpp ====
LedController ledController;
WebUi web;
BambuMqttClient mqttClient;
PrinterStateMachine printerState;

namespace {
constexpr unsigned long kFrameIntervalMs = 33;  // ~30fps animation refresh
unsigned long lastFrameMs = 0;
bool bootHeld = false;
unsigned long bootHoldStart = 0;
unsigned long idleSinceMs = 0;
bool asleep = false;
uint32_t appliedRevision = 0;
String boundIp;
String boundSerial;
String boundCode;

const char* stateKey(SystemState state) {
  switch (state) {
    case SystemState::Provisioning:
      return "provisioning";
    case SystemState::WifiConnecting:
      return "wifi";
    case SystemState::MqttConnecting:
      return "mqtt";
    case SystemState::Idle:
      return "idle";
    case SystemState::Homing:
      return "homing";
    case SystemState::Leveling:
      return "leveling";
    case SystemState::Downloading:
      return "downloading";
    case SystemState::Preparing:
      return "preparing";
    case SystemState::Heating:
      return "heating";
    case SystemState::Printing:
      return "printing";
    case SystemState::Paused:
      return "paused";
    case SystemState::Finished:
      return "finished";
    case SystemState::Error:
      return "error";
  }
  return "unknown";
}

SystemState visualState(bool staUp, bool printerReady, bool mqttUp) {
  if (!staUp) return SystemState::WifiConnecting;
  if (!printerReady) return SystemState::Provisioning;
  if (!mqttUp) return SystemState::MqttConnecting;
  return printerState.state();
}

void applyLedStyle() {
  const DeviceSettings& cfg = web.settings();
  auto slot = [](const LightSlot& s) {
    LedStyleSlot out;
    out.color = CRGB(s.r, s.g, s.b);
    out.effect = s.effect;
    return out;
  };
  LedStyle style;
  style.brightness = cfg.brightness;
  style.speed = cfg.speed;
  style.idle = slot(cfg.idle);
  style.printing = slot(cfg.printing);
  style.error = slot(cfg.error);
  style.homing = slot(cfg.homing);
  style.leveling = slot(cfg.leveling);
  style.preparing = slot(cfg.preparing);
  style.heating = slot(cfg.heating);
  style.downloading = slot(cfg.downloading);
  style.paused = slot(cfg.paused);
  style.finished = slot(cfg.finished);
  style.reverse = cfg.ledReverse;
  ledController.setStyle(style);
}

void pollFactoryButton(unsigned long now) {
  if (web.otaBusy()) {
    bootHeld = false;
    return;
  }
  if (digitalRead(hw::kResetButtonPin) == LOW) {
    if (!bootHeld) {
      bootHeld = true;
      bootHoldStart = now;
    } else if (now - bootHoldStart >= hw::kResetHoldMs) {
      Serial.println(F("BOOT held — factory reset"));
      web.factoryReset();
    }
  } else {
    bootHeld = false;
  }
}

void onMqttPayload(ArduinoJson::JsonVariantConst printObj, unsigned long nowMs) {
  printerState.update(printObj, nowMs);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(hw::kResetButtonPin, INPUT_PULLUP);

  ledController.begin();
  web.begin();
  web.onOtaStart([]() { mqttClient.disconnect(); });
  esp_ota_mark_app_valid_cancel_rollback();

  Serial.print(F("Bambu Status Bar v"));
  Serial.println(kFirmwareVersion);
}

void loop() {
  unsigned long now = millis();
  const DeviceSettings& cfg = web.settings();

  if (appliedRevision != web.revision()) {
    appliedRevision = web.revision();
    applyLedStyle();
  }

  if (!web.printerReady()) {
    boundIp = "";
    boundSerial = "";
    boundCode = "";
  } else if (web.staConnected() && (boundIp != cfg.printerIp || boundSerial != cfg.printerSerial ||
                                    boundCode != cfg.accessCode)) {
    boundIp = cfg.printerIp;
    boundSerial = cfg.printerSerial;
    boundCode = cfg.accessCode;
    mqttClient.begin(cfg.printerIp, mqtt_cfg::kPort, mqtt_cfg::kUsername, cfg.accessCode, cfg.printerSerial);
    mqttClient.setPayloadHandler(onMqttPayload);
    Serial.println(F("Printer saved — connecting MQTT..."));
  }

  // Serve the web UI before any MQTT/TLS work. connect() is still blocking,
  // but timeouts keep the stall short; LEDs refresh around it.
  web.loop();
  pollFactoryButton(now);

  if (web.staConnected() && web.printerReady() && !web.otaBusy()) {
    mqttClient.ensureConnected(now);
    mqttClient.loop();
  }

  bool mqttUp = web.printerReady() && mqttClient.connected();
  SystemState shown = visualState(web.staConnected(), web.printerReady(), mqttUp);

  bool idleVisual = shown == SystemState::Idle ||
                    (shown == SystemState::Finished && printerState.telemetry().finishedAtMs != 0 &&
                     now - printerState.telemetry().finishedAtMs >= led_cfg::kFinishedSolidMs);
  bool activity = web.consumeActivity();
  if (!idleVisual) {
    asleep = false;
    idleSinceMs = now;
  } else if (activity) {
    asleep = false;
    idleSinceMs = now;
  } else if (now - idleSinceMs >= led_cfg::kStandbyAfterIdleMs) {
    asleep = true;
  }
  ledController.setAsleep(asleep);

  const char* reported = idleVisual ? "idle" : stateKey(shown);
  web.setLive(reported, printerState.telemetry().progressPercent, asleep && idleVisual, mqttUp);

  if (now - lastFrameMs >= kFrameIntervalMs) {
    lastFrameMs = now;
    ledController.render(shown, printerState.telemetry(), now);
  }
}
