// GENERATED FILE — do not edit directly.
// Source of truth is src/*.{h,cpp}; regenerate with:
//   python3 tools/generate_arduino_sketch.py
//
// Headers live beside this .ino (copied from src/) so Arduino IDE
// function prototypes are emitted after enum/class declarations.

#include <cstdint>
#include <sdkconfig.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <FastLED.h>
#include <math.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <AsyncUDP.h>
#include <cstdio>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <cctype>
#include <vector>
#include <esp_ota_ops.h>

#include "Config.h"
#include "NetHelpers.h"
#include "PrinterState.h"
#include "LedEffects.h"
#include "LedController.h"
#include "BambuMqttClient.h"
#include "Settings.h"
#include "GithubOta.h"
#include "SsdpAdvertiser.h"
#include "WebPage.h"
#include "WebUi.h"

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

  updateChamberLight(printObj);

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

void PrinterStateMachine::updateChamberLight(JsonVariantConst printObj) {
  // Diff payloads often omit lights_report — keep the last known chamber state.
  if (printObj["lights_report"].isNull()) return;
  JsonArrayConst lights = printObj["lights_report"].as<JsonArrayConst>();
  if (lights.isNull()) return;
  for (JsonVariantConst entry : lights) {
    const char* node = entry["node"] | "";
    if (strcmp(node, "chamber_light") != 0) continue;
    const char* mode = entry["mode"] | "";
    // on and flashing both mean the printer light is active.
    chamberLightOn_ = (strcmp(mode, "on") == 0 || strcmp(mode, "flashing") == 0);
    haveChamberLight_ = true;
    break;
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

// ==== LedController.cpp ====
namespace {
uint8_t brightnessToPwm(uint8_t percent) {
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((static_cast<uint16_t>(percent) * 255) / 100);
}
}  // namespace

bool LedController::addLedsForPin(uint8_t pin) {
#define LED_PIN_CASE(p)                              \
  case p:                                            \
    FastLED.addLeds<WS2812B, p, GRB>(leds_, count_); \
    return true
  switch (pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    LED_PIN_CASE(0);
    LED_PIN_CASE(1);
    LED_PIN_CASE(2);
    LED_PIN_CASE(3);
    LED_PIN_CASE(4);
    LED_PIN_CASE(5);
    LED_PIN_CASE(6);
    LED_PIN_CASE(7);
    LED_PIN_CASE(8);
    LED_PIN_CASE(9);
    LED_PIN_CASE(10);
    LED_PIN_CASE(11);
    LED_PIN_CASE(14);
    LED_PIN_CASE(15);
    LED_PIN_CASE(18);
    LED_PIN_CASE(19);
    LED_PIN_CASE(20);
    LED_PIN_CASE(21);
    LED_PIN_CASE(22);
    LED_PIN_CASE(23);
#elif defined(CONFIG_IDF_TARGET_ESP32)
    LED_PIN_CASE(0);
    LED_PIN_CASE(2);
    LED_PIN_CASE(4);
    LED_PIN_CASE(5);
    LED_PIN_CASE(12);
    LED_PIN_CASE(13);
    LED_PIN_CASE(14);
    LED_PIN_CASE(15);
    LED_PIN_CASE(16);
    LED_PIN_CASE(17);
    LED_PIN_CASE(18);
    LED_PIN_CASE(19);
    LED_PIN_CASE(21);
    LED_PIN_CASE(22);
    LED_PIN_CASE(23);
    LED_PIN_CASE(25);
    LED_PIN_CASE(26);
    LED_PIN_CASE(27);
    LED_PIN_CASE(32);
    LED_PIN_CASE(33);
#endif
    default:
      FastLED.addLeds<WS2812B, hw::kDefaultDataPin, GRB>(leds_, count_);
      return false;
  }
#undef LED_PIN_CASE
}

void LedController::begin(uint8_t dataPin, uint16_t ledCount) {
  count_ = ledCount < 1 ? 1 : ledCount;
  if (count_ > hw::kMaxNumLeds) count_ = hw::kMaxNumLeds;
  if (!hw::isSupportedLedDataPin(dataPin)) dataPin = hw::kDefaultDataPin;
  addLedsForPin(dataPin);
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
    for (int i = 0, j = static_cast<int>(count_) - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
  FastLED.show();
  // Restore logical order so stateful effects (sparkle/ember) keep working.
  if (style_.reverse) {
    for (int i = 0, j = static_cast<int>(count_) - 1; i < j; ++i, --j) {
      CRGB tmp = leds_[i];
      leds_[i] = leds_[j];
      leds_[j] = tmp;
    }
  }
}

void LedController::showBlack() {
  fill_solid(leds_, count_, CRGB::Black);
  showLeds();
}

void LedController::renderSolid(CRGB color) {
  fill_solid(leds_, count_, color);
  showLeds();
}

void LedController::renderProgressBar(CRGB onColor, CRGB offColor, uint8_t percent) {
  int onLeds = static_cast<int>(count_ * (percent / 100.0f) + 0.5f);
  for (int i = 0; i < static_cast<int>(count_); i++) {
    leds_[i] = (i < onLeds) ? onColor : offColor;
  }
  showLeds();
}

void LedController::renderHeating(uint8_t percent) {
  int lit = static_cast<int>(count_ * (percent / 100.0f) + 0.5f);
  fill_solid(leds_, count_, CRGB(30, 0, 0));
  for (int i = 0; i < lit; i++) {
    leds_[i] = CRGB(map(i, 0, max(lit - 1, 1), 64, 255), map(i, 0, max(lit - 1, 1), 21, 85), 0);
  }
  showLeds();
}

void LedController::renderComet(CRGB color, unsigned long nowMs, uint16_t stepMs) {
  // Knight Rider scanner: linear ping-pong + symmetric soft glow (no directional trail to flip).
  fill_solid(leds_, count_, CRGB::Black);
  if (count_ == 0) {
    showLeds();
    return;
  }
  if (stepMs < 8) stepMs = 8;
  if (count_ == 1) {
    leds_[0] = color;
    showLeds();
    return;
  }

  const float span = static_cast<float>(count_ - 1);
  const uint32_t halfTrip = static_cast<uint32_t>(span * static_cast<float>(stepMs));
  const uint32_t roundTrip = halfTrip * 2;
  const uint32_t t = nowMs % roundTrip;
  // Constant-speed triangle wave — classic KITT pacing.
  const float pos = (t <= halfTrip)
                        ? static_cast<float>(t) / static_cast<float>(stepMs)
                        : span - static_cast<float>(t - halfTrip) / static_cast<float>(stepMs);

  // Soft eye width scales with strip length; keep a readable glow on short bars.
  float halfWidth = span * 0.28f;
  if (halfWidth < 2.5f) halfWidth = 2.5f;
  if (halfWidth > 7.0f) halfWidth = 7.0f;

  for (int i = 0; i < static_cast<int>(count_); i++) {
    const float d = fabsf(static_cast<float>(i) - pos);
    if (d >= halfWidth) continue;
    const float u = 1.0f - (d / halfWidth);
    // Quadratic falloff = bright core, soft flanks (Knight Rider look).
    const float bri = u * u;
    CRGB c = color;
    c.nscale8(static_cast<uint8_t>(bri * 255.0f));
    leds_[i] = c;
  }
  showLeds();
}

void LedController::renderWave(unsigned long nowMs, uint8_t hue) {
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  // One sine period across the whole strip (single crest), independent of LED count.
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint8_t v = sin8(static_cast<uint8_t>((i * 256) / count_ + phase * 40));
    leds_[i] = CHSV(hue, 255, v);
  }
  showLeds();
}

void LedController::renderWaveColor(CRGB color, unsigned long nowMs) {
  // Scale the chosen RGB color — do not force HSV sat=255 (white → hue 0 = red).
  float speed = style_.speed / 50.0f;
  float phase = nowMs * 0.002f * speed;
  // One sine period across the whole strip (single crest), independent of LED count.
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint8_t v = sin8(static_cast<uint8_t>((i * 256) / count_ + phase * 40));
    CRGB lit = color;
    lit.nscale8(v);
    leds_[i] = lit;
  }
  showLeds();
}

void LedController::renderPulse(CRGB color, unsigned long nowMs, uint16_t periodMs) {
  // Smooth sine breathe with a fixed dark hold at the bottom of each cycle.
  if (periodMs < 100) periodMs = 100;
  uint16_t darkHold = 250;
  if (darkHold > (periodMs * 3) / 10) darkHold = (periodMs * 3) / 10;
  const uint16_t breathMs = periodMs - darkHold;
  const uint16_t t = nowMs % periodMs;

  uint8_t scale = 0;
  if (t >= darkHold && breathMs > 0) {
    const float u = static_cast<float>(t - darkHold) / static_cast<float>(breathMs);
    // sin(pi*u): 0 → 1 → 0 with soft peak, no linear kink
    const float env = sinf(u * 3.14159265f);
    scale = static_cast<uint8_t>(env * 255.0f);
  }
  CRGB col = color;
  col.nscale8(scale);
  fill_solid(leds_, count_, col);
  showLeds();
}

void LedController::renderStrobe(CRGB color, unsigned long nowMs, uint16_t onMs, uint16_t offMs) {
  uint16_t t = nowMs % (onMs + offMs);
  fill_solid(leds_, count_, t < onMs ? color : CRGB::Black);
  showLeds();
}

void LedController::renderRainbow(unsigned long nowMs) {
  uint8_t speed = style_.speed < 1 ? 1 : style_.speed;
  uint8_t base = static_cast<uint8_t>((nowMs / (20 + (100 - speed))) & 0xFF);
  const int step = count_ > 0 ? (256 / count_) : 0;
  for (int i = 0; i < static_cast<int>(count_); i++) {
    leds_[i] = CHSV(static_cast<uint8_t>(base + i * step), 255, 255);
  }
  showLeds();
}

void LedController::renderSparkle(CRGB color, unsigned long nowMs) {
  static unsigned long lastStep = 0;
  fadeToBlackBy(leds_, count_, 40);
  if (nowMs - lastStep >= scaled(40)) {
    lastStep = nowMs;
    int sparks = 1 + (style_.speed / 40);
    for (int s = 0; s < sparks; s++) {
      leds_[random16(count_)] = color;
    }
  }
  showLeds();
}

void LedController::renderEmber(unsigned long nowMs) {
  (void)nowMs;
  // Horizontal glow: cool, blur left/right, spark along the strip (not "up").
  for (int i = 0; i < static_cast<int>(count_); i++) {
    heat_[i] = qsub8(heat_[i], random8(1, 10));
  }

  uint8_t next[hw::kMaxNumLeds];
  for (int i = 0; i < static_cast<int>(count_); i++) {
    uint16_t sum = static_cast<uint16_t>(heat_[i]) * 2;
    sum += (i > 0) ? heat_[i - 1] : heat_[i];
    sum += (i + 1 < static_cast<int>(count_)) ? heat_[i + 1] : heat_[i];
    next[i] = static_cast<uint8_t>(sum / 4);
  }
  for (int i = 0; i < static_cast<int>(count_); i++) {
    heat_[i] = next[i];
  }

  const uint8_t sparkChance = static_cast<uint8_t>(28 + style_.speed / 2);
  if (count_ > 0 && random8() < sparkChance) {
    const int x = random16(count_);
    heat_[x] = qadd8(heat_[x], random8(140, 255));
    if (x > 0) heat_[x - 1] = qadd8(heat_[x - 1], random8(30, 90));
    if (x + 1 < static_cast<int>(count_)) heat_[x + 1] = qadd8(heat_[x + 1], random8(30, 90));
  }

  for (int j = 0; j < static_cast<int>(count_); j++) {
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
    case LedEffect::Ember:
      renderEmber(nowMs);
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
      nowMs - telemetry.finishedAtMs >= finishedHoldMs_) {
    state = SystemState::Idle;
  }

  // asleep_ covers standby idle and optional chamber-light follow (any state).
  if (asleep_) {
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

// ==== GithubOta.cpp ====
namespace {
constexpr const char* kUserAgent = "BambuStatusBar-OTA";
// Heap-backed TLS + HTTP on a dedicated task. Stack only holds locals/control.
constexpr uint32_t kTlsTaskStackBytes = 32768;
constexpr uint32_t kCheckTimeoutMs = 60000;
constexpr uint32_t kInstallTimeoutMs = 300000;

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

void setHttpError(String& errorOut, HTTPClient& http, int code) {
  Serial.print(F("OTA HTTP "));
  Serial.print(code);
  Serial.print(F(" "));
  Serial.print(http.errorToString(code));
  Serial.print(F(" heap="));
  Serial.println(ESP.getFreeHeap());
  errorOut = (code < 0) ? String("http-connect") : (String("http-") + code);
}

void preferPublicDns() {
  // Fritzbox / captive DNS can fail for public names; pin resolvers for OTA.
  WiFi.setDNS(IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
}

bool resolveIpv4(const char* host, IPAddress& out) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const int err = getaddrinfo(host, nullptr, &hints, &res);
  if (err != 0 || !res) {
    Serial.print(F("OTA getaddrinfo failed "));
    Serial.println(err);
    return false;
  }
  const auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
  out = IPAddress(sin->sin_addr.s_addr);
  freeaddrinfo(res);
  return true;
}

WiFiClientSecure* makeSecureClient(uint32_t handshakeSec, uint32_t timeoutMs) {
  // Heap allocation: SSL state is too large for the task stack.
  auto* client = new WiFiClientSecure();
  if (!client) return nullptr;
  client->setInsecure();
  client->setHandshakeTimeout(handshakeSec);
  client->setTimeout(timeoutMs);
  return client;
}

bool httpGetToString(const char* host, const String& uri, String& body, String& errorOut) {
  preferPublicDns();

  IPAddress ip;
  if (!resolveIpv4(host, ip)) {
    // Fall back to Arduino helper (may return IPv6 on some stacks).
    if (!WiFi.hostByName(host, ip)) {
      errorOut = "http-connect";
      return false;
    }
  }
  Serial.print(F("OTA DNS "));
  Serial.print(host);
  Serial.print(F(" -> "));
  Serial.println(ip);

  WiFiClientSecure* client = makeSecureClient(25, 30000);
  if (!client) {
    errorOut = "http-connect";
    return false;
  }

  // Dial IPv4 with SNI hostname (avoids broken AAAA paths on some routers).
  if (!client->connect(ip, 443, host, nullptr, nullptr, nullptr)) {
    Serial.println(F("OTA TLS connect failed"));
    delete client;
    errorOut = "http-connect";
    return false;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(30000);
  if (!http.begin(*client, host, 443, uri, /*https=*/true)) {
    errorOut = "http-begin";
    http.end();
    delete client;
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setHttpError(errorOut, http, code);
    http.end();
    delete client;
    return false;
  }
  body = http.getString();
  http.end();
  delete client;
  return true;
}

GithubReleaseInfo checkLatestImpl() {
  GithubReleaseInfo info;
  info.currentVersion = kFirmwareVersion;

  if (WiFi.status() != WL_CONNECTED) {
    info.error = "no-wifi";
    return info;
  }

  Serial.print(F("OTA check heap="));
  Serial.println(ESP.getFreeHeap());

  const String uri =
      String("/repos/") + kGithubOwner + "/" + kGithubRepo + "/releases/latest";
  String body;
  if (!httpGetToString("api.github.com", uri, body, info.error)) return info;

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

bool installFromUrlImpl(const String& downloadUrl, String& errorOut) {
  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "no-wifi";
    return false;
  }
  if (downloadUrl.isEmpty()) {
    errorOut = "no-url";
    return false;
  }

  preferPublicDns();

  WiFiClientSecure* client = makeSecureClient(30, 60000);
  if (!client) {
    errorOut = "http-connect";
    return false;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(60000);
  if (!http.begin(*client, downloadUrl)) {
    errorOut = "http-begin";
    delete client;
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setHttpError(errorOut, http, code);
    http.end();
    delete client;
    return false;
  }

  const int contentLength = http.getSize();
  WiFiClient& stream = http.getStream();
  if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN)) {
    errorOut = "update-begin";
    http.end();
    delete client;
    return false;
  }

  const size_t written = Update.writeStream(stream);
  if (contentLength > 0 && written != static_cast<size_t>(contentLength)) {
    Update.abort();
    errorOut = "short-write";
    http.end();
    delete client;
    return false;
  }
  if (!Update.end(true)) {
    errorOut = "update-end";
    http.end();
    delete client;
    return false;
  }
  http.end();
  delete client;
  return true;
}

GithubReleaseInfo s_checkResult;
volatile bool s_checkDone = false;

String s_installUrl;
String s_installError;
volatile bool s_installOk = false;
volatile bool s_installDone = false;

void checkTask(void* /*arg*/) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    s_checkResult = checkLatestImpl();
    if (s_checkResult.ok || s_checkResult.error == "no-wifi") break;
    Serial.print(F("OTA check retry "));
    Serial.println(attempt + 1);
    preferPublicDns();
    vTaskDelay(pdMS_TO_TICKS(800 * (attempt + 1)));
  }
  s_checkDone = true;
  vTaskDelete(nullptr);
}

void installTask(void* /*arg*/) {
  s_installOk = installFromUrlImpl(s_installUrl, s_installError);
  s_installDone = true;
  vTaskDelete(nullptr);
}

bool waitFlag(volatile bool& done, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (!done) {
    if (millis() - start > timeoutMs) return false;
    delay(10);
    yield();
  }
  return true;
}
}  // namespace

GithubReleaseInfo GithubOta::checkLatest() {
  s_checkDone = false;
  s_checkResult = GithubReleaseInfo{};
  s_checkResult.currentVersion = kFirmwareVersion;

  if (xTaskCreate(checkTask, "ghCheck", kTlsTaskStackBytes, nullptr, 1, nullptr) != pdPASS) {
    return checkLatestImpl();
  }
  if (!waitFlag(s_checkDone, kCheckTimeoutMs)) {
    Serial.println(F("OTA check task timeout"));
    s_checkResult.ok = false;
    s_checkResult.error = "http-connect";
    s_checkResult.currentVersion = kFirmwareVersion;
  }
  return s_checkResult;
}

bool GithubOta::installFromUrl(const String& downloadUrl, String& errorOut) {
  s_installUrl = downloadUrl;
  s_installError = "";
  s_installOk = false;
  s_installDone = false;

  if (xTaskCreate(installTask, "ghOta", kTlsTaskStackBytes, nullptr, 1, nullptr) != pdPASS) {
    return installFromUrlImpl(downloadUrl, errorOut);
  }
  if (!waitFlag(s_installDone, kInstallTimeoutMs)) {
    Serial.println(F("OTA install task timeout"));
    errorOut = "http-connect";
    return false;
  }
  errorOut = s_installError;
  return s_installOk;
}

// ==== SsdpAdvertiser.cpp ====
namespace {
constexpr uint16_t kSsdpPort = 1900;
constexpr uint8_t kSsdpTtl = 2;
constexpr unsigned long kCacheSeconds = 1800;
constexpr unsigned long kNotifyFastMs = 60UL * 1000UL;
constexpr unsigned long kNotifySlowMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kNotifyFastWindowMs = 5UL * 60UL * 1000UL;
const IPAddress kSsdpMulticast(239, 255, 255, 250);

bool wantsRoot(const String& st) {
  return st.equalsIgnoreCase("ssdp:all") || st.equalsIgnoreCase("upnp:rootdevice") ||
         st.equalsIgnoreCase("urn:schemas-upnp-org:device:Basic:1");
}
}  // namespace

void SsdpAdvertiser::configure(const String& friendlyName, const String& uuid) {
  friendlyName_ = friendlyName.length() ? friendlyName : String(kDefaultHostname);
  uuid_ = uuid;
  if (!uuid_.isEmpty() && !uuid_.startsWith("uuid:")) uuid_ = String("uuid:") + uuid_;
}

void SsdpAdvertiser::begin(const String& friendlyName, const String& uuid) {
  stop();
  configure(friendlyName, uuid);
  if (uuid_.isEmpty()) return;

  // TTL=2 matches ESP32SSDP; WiFiUDP multicast often fails to be noticed by AVM.
  if (!udp_.listenMulticast(kSsdpMulticast, kSsdpPort, kSsdpTtl)) {
    Serial.println(F("SSDP multicast join failed"));
    return;
  }
  udp_.onPacket([this](AsyncUDPPacket packet) { onPacket(packet); });
  running_ = true;
  startedMs_ = millis();
  nextNotifyMs_ = 0;
  sendNotifyAll();
  Serial.print(F("SSDP advertising "));
  Serial.println(locationUrl());
}

void SsdpAdvertiser::stop() {
  if (!running_) return;
  running_ = false;
  udp_.close();
}

void SsdpAdvertiser::setFriendlyName(const String& friendlyName) {
  if (friendlyName.length()) friendlyName_ = friendlyName;
}

void SsdpAdvertiser::loop() {
  if (!running_) return;
  if (WiFi.status() != WL_CONNECTED) {
    stop();
    return;
  }
  if (static_cast<long>(millis() - nextNotifyMs_) >= 0) sendNotifyAll();
}

String SsdpAdvertiser::locationUrl() const {
  return String("http://") + WiFi.localIP().toString() + "/description.xml";
}

String SsdpAdvertiser::usn(const char* st) const {
  String out = uuid_;
  if (st && st[0]) {
    out += "::";
    out += st;
  }
  return out;
}

String SsdpAdvertiser::schemaXml() const {
  // Relative presentationURL (ESP32SSDP style); Fritzbox joins it with URLBase.
  const String name = friendlyName_.length() ? friendlyName_ : String(kDefaultHostname);
  const String udn = uuid_.length() ? uuid_ : String("uuid:00000000-0000-0000-0000-000000000000");
  const String ip = WiFi.localIP().toString();
  const String base = String("http://") + ip + ":80/";
  String xml;
  xml.reserve(900);
  xml += F("<?xml version=\"1.0\"?>\r\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
           "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n");
  xml += F("<URLBase>");
  xml += base;
  xml += F("</URLBase>\r\n<device>\r\n"
           "<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\r\n"
           "<friendlyName>");
  xml += name;
  xml += F("</friendlyName>\r\n"
           "<manufacturer>Bambu Status Bar</manufacturer>\r\n"
           "<manufacturerURL>https://github.com/");
  xml += kGithubOwner;
  xml += '/';
  xml += kGithubRepo;
  xml += F("</manufacturerURL>\r\n"
           "<modelDescription>LED status bar for Bambu Lab printers</modelDescription>\r\n"
           "<modelName>Bambu Status Bar</modelName>\r\n"
           "<modelNumber>");
  xml += kFirmwareVersion;
  xml += F("</modelNumber>\r\n"
           "<modelURL>https://github.com/");
  xml += kGithubOwner;
  xml += '/';
  xml += kGithubRepo;
  xml += F("</modelURL>\r\n"
           "<serialNumber>");
  xml += WiFi.macAddress();
  xml += F("</serialNumber>\r\n"
           "<UDN>");
  xml += udn;
  xml += F("</UDN>\r\n"
           "<presentationURL>/</presentationURL>\r\n"
           "</device>\r\n</root>\r\n");
  return xml;
}

void SsdpAdvertiser::sendNotifyAll() {
  if (!running_ || WiFi.status() != WL_CONNECTED) return;
  // ESP32SSDP / Windows / AVM expect root + uuid + device-type alive signals.
  sendNotify("upnp:rootdevice");
  sendNotify(uuid_.c_str());
  sendNotify("urn:schemas-upnp-org:device:Basic:1");
  const unsigned long age = millis() - startedMs_;
  nextNotifyMs_ = millis() + (age < kNotifyFastWindowMs ? kNotifyFastMs : kNotifySlowMs);
}

void SsdpAdvertiser::sendNotify(const char* nt) {
  if (!nt || !nt[0]) return;
  const String loc = locationUrl();
  const String ntsUsn = (strncmp(nt, "uuid:", 5) == 0) ? String(nt) : usn(nt);
  char buf[640];
  snprintf(buf, sizeof(buf),
           "NOTIFY * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "CACHE-CONTROL: max-age=%lu\r\n"
           "LOCATION: %s\r\n"
           "NT: %s\r\n"
           "NTS: ssdp:alive\r\n"
           "SERVER: Arduino/1.0 UPnP/1.1 BambuStatusBar/%s\r\n"
           "USN: %s\r\n"
           "\r\n",
           static_cast<unsigned long>(kCacheSeconds), loc.c_str(), nt, kFirmwareVersion,
           ntsUsn.c_str());
  udp_.writeTo(reinterpret_cast<const uint8_t*>(buf), strlen(buf), kSsdpMulticast, kSsdpPort);
}

void SsdpAdvertiser::sendSearchResponse(const IPAddress& addr, uint16_t port, const char* st) {
  if (!running_ || WiFi.status() != WL_CONNECTED) return;
  const String loc = locationUrl();
  const String responseSt = st && st[0] ? String(st) : String("upnp:rootdevice");
  const String responseUsn =
      responseSt.startsWith("uuid:") ? responseSt : usn(responseSt.c_str());
  char buf[640];
  snprintf(buf, sizeof(buf),
           "HTTP/1.1 200 OK\r\n"
           "CACHE-CONTROL: max-age=%lu\r\n"
           "EXT:\r\n"
           "LOCATION: %s\r\n"
           "SERVER: Arduino/1.0 UPnP/1.1 BambuStatusBar/%s\r\n"
           "ST: %s\r\n"
           "USN: %s\r\n"
           "\r\n",
           static_cast<unsigned long>(kCacheSeconds), loc.c_str(), kFirmwareVersion,
           responseSt.c_str(), responseUsn.c_str());
  udp_.writeTo(reinterpret_cast<const uint8_t*>(buf), strlen(buf), addr, port);
}

void SsdpAdvertiser::onPacket(AsyncUDPPacket& packet) {
  if (!running_ || packet.length() == 0) return;
  const size_t len = packet.length() < 512 ? packet.length() : 512;
  char raw[513];
  memcpy(raw, packet.data(), len);
  raw[len] = 0;
  String text(raw);
  if (text.indexOf("M-SEARCH") < 0) return;
  String st = httpHeaderValue(text, "ST");
  if (st.isEmpty()) return;
  if (st.equalsIgnoreCase(uuid_)) {
    sendSearchResponse(packet.remoteIP(), packet.remotePort(), st.c_str());
    return;
  }
  if (!wantsRoot(st)) return;
  const char* replySt = st.equalsIgnoreCase("ssdp:all") ? "upnp:rootdevice" : st.c_str();
  sendSearchResponse(packet.remoteIP(), packet.remotePort(), replySt);
}

// ==== WebUi.cpp ====
struct WebUi::Impl {
  WebServer server{80};
  DNSServer dns;
  WiFiUDP udp;
  SsdpAdvertiser ssdp;
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
    // Underscores are allowed in the stored name; dhcpHostname() maps them to
    // hyphens for the wire (Fritzbox-safe).
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

}  // namespace

void WebUi::begin() {
  static Impl storage;
  impl_ = &storage;
  store_.begin();

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
  impl_->server.on("/description.xml", HTTP_GET, [this]() {
    noteActivity();
    impl_->ssdp.configure(store_.data().hostname, deviceUuid());
    impl_->server.send(200, "text/xml", impl_->ssdp.schemaXml());
  });
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
  if (staConnected_) {
    if (!mdnsStarted_ && static_cast<long>(millis() - mdnsRetryAt_) >= 0) startMdns();
    if (!impl_->ssdp.running()) startSsdp();
    impl_->ssdp.loop();
  } else {
    if (mdnsStarted_) {
      MDNS.end();
      mdnsStarted_ = false;
    }
    mdnsRetryAt_ = 0;
    stopSsdp();
  }
  syncAp(staConnected_);
  pollBootUpdateCheck();
}

void WebUi::setLive(const char* state, uint8_t progress, bool asleep, bool mqttConnected) {
  liveState_ = state ? state : "unknown";
  liveProgress_ = progress;
  liveAsleep_ = asleep;
  liveMqtt_ = mqttConnected;
}

void WebUi::setChamberLight(bool known, bool on) {
  liveChamberLight_ = known ? (on ? "on" : "off") : "unknown";
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
  if (wire.isEmpty()) wire = kDefaultHostname;
  wire.replace('_', '-');
  if (wire.length() > 31) wire = wire.substring(0, 31);
  while (wire.endsWith("-")) wire.remove(wire.length() - 1);
  if (wire.isEmpty()) wire = kDefaultHostname;
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
  // Captive DNS on :53 is only for offline setup. While STA is up it can
  // interfere with outbound hostname lookups (GitHub OTA, etc.).
  if (WiFi.status() != WL_CONNECTED) {
    impl_->dns.start(53, "*", WiFi.softAPIP());
  }
  apRunning_ = true;
}

void WebUi::stopAp() {
  impl_->dns.stop();
  WiFi.softAPdisconnect(true);
  apRunning_ = false;
}

void WebUi::syncAp(bool staUp) {
  // Keep SoftAP off for the whole GitHub HTTPS window (RAM + cleaner DNS).
  if (otaBusy_) return;
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

void WebUi::syncCaptiveDns(bool staUp) {
  if (!apRunning_ || !impl_) return;
  // Only hijack DNS while offline (setup hotspot). With STA up, leave :53 free
  // so api.github.com and other public hosts can resolve for OTA.
  impl_->dns.stop();
  if (!staUp) impl_->dns.start(53, "*", WiFi.softAPIP());
}

void WebUi::prepareGithubHttps() {
  // Free MQTT TLS, pause SoftAP/captive DNS, and pin public resolvers so
  // api.github.com can be reached reliably on ESP32-C6 AP+STA setups.
  if (onOtaStart_) onOtaStart_();
  stopSsdp();
  if (impl_) impl_->dns.stop();
  if (apRunning_) {
    WiFi.softAPdisconnect(true);
    apRunning_ = false;
    apPausedForOta_ = true;
  }
  WiFi.setDNS(IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
  delay(700);
}

void WebUi::finishGithubHttps() {
  if (!apPausedForOta_) return;
  apPausedForOta_ = false;
  if (store_.data().apEnabled || !staConnected_) startAp();
}

void WebUi::pollBootUpdateCheck() {
  if (!staConnected_) {
    staUpSinceMs_ = 0;
    return;
  }
  if (staUpSinceMs_ == 0) staUpSinceMs_ = millis();
  if (bootUpdateCheckDone_ || bootUpdateCheckRunning_ || otaBusy_) return;
  if (millis() - staUpSinceMs_ < ota_cfg::kBootCheckDelayMs) return;

  bootUpdateCheckRunning_ = true;
  otaBusy_ = true;
  prepareGithubHttps();
  if (xTaskCreate(&WebUi::bootUpdateCheckTask, "bootOta", 4096, this, 1, nullptr) != pdPASS) {
    bootUpdateCheckRunning_ = false;
    otaBusy_ = false;
    bootUpdateCheckDone_ = true;
    Serial.println(F("Boot update check: task create failed"));
  }
}

void WebUi::bootUpdateCheckTask(void* arg) {
  auto* self = static_cast<WebUi*>(arg);
  const GithubReleaseInfo info = GithubOta::checkLatest();
  self->finishBootUpdateCheck(info.ok, info.ok && info.updateAvailable, info.latestVersion,
                               info.downloadUrl);
  vTaskDelete(nullptr);
}

void WebUi::finishBootUpdateCheck(bool ok, bool available, const String& latest, const String& url) {
  updateAvailable_ = available;
  latestVersion_ = latest;
  updateUrl_ = url;
  bootUpdateCheckDone_ = true;
  bootUpdateCheckRunning_ = false;
  otaBusy_ = false;
  finishGithubHttps();
  Serial.print(F("Boot update check "));
  if (!ok) {
    Serial.println(F("failed"));
    return;
  }
  if (available) {
    Serial.print(F("available -> "));
    Serial.println(latest);
  } else {
    Serial.println(F("up to date"));
  }
}

void WebUi::startMdns() {
  // Advertise the DHCP-safe label (no underscores) as hostname and instance,
  // with _http._tcp on port 80.
  const String host = dhcpHostname();
  if (!MDNS.begin(host.c_str())) {
    mdnsRetryAt_ = millis() + 5000;
    return;
  }
  MDNS.setInstanceName(host.c_str());
  MDNS.addService("http", "tcp", 80);
  mdnsStarted_ = true;
  mdnsRetryAt_ = 0;
  Serial.print(F("mDNS http://"));
  Serial.print(host);
  Serial.println(F(".local"));
}

String WebUi::deviceUuid() const {
  // Stable UPnP UDN from the STA MAC (same form many ESP SSDP examples use).
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[40];
  snprintf(buf, sizeof(buf), "38323636-4558-4dda-9188-cda0e6%02x%02x%02x", mac[3], mac[4],
           mac[5]);
  return String(buf);
}

void WebUi::startSsdp() {
  if (!impl_ || !staConnected_) return;
  // Friendly name can keep underscores for the Fritzbox label; UUID is MAC-derived.
  String name = store_.data().hostname;
  if (name.isEmpty()) name = dhcpHostname();
  impl_->ssdp.begin(name, deviceUuid());
}

void WebUi::stopSsdp() {
  if (!impl_) return;
  impl_->ssdp.stop();
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
  const bool upNow = WiFi.status() == WL_CONNECTED;
  if (upNow != staConnected_) {
    staConnected_ = upNow;
    syncCaptiveDns(staConnected_);
  }
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

  String ip = httpHeaderValue(text, "location");
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

  String serial = httpHeaderValue(text, "usn");
  int sep = serial.indexOf("::");
  if (sep >= 0) serial = serial.substring(0, sep);
  serial.replace("uuid:", "");
  serial.trim();
  // Bambu USN is the bare printer serial (alphanumeric). Reject UUID-like junk.
  if (serial.indexOf('-') >= 0 || serial.indexOf(':') >= 0 || serial.indexOf('.') >= 0 ||
      serial.length() < 8 || serial.length() > 32) {
    serial = "";
  }

  String name = httpHeaderValue(text, "DevName.bambu.com");
  if (name.isEmpty()) name = httpHeaderValue(text, "devname.bambu.com");
  if (name.isEmpty()) name = httpHeaderValue(text, "devname");

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
  {
    JsonObject update = doc["update"].to<JsonObject>();
    update["checked"] = bootUpdateCheckDone_;
    update["checking"] = bootUpdateCheckRunning_;
    update["available"] = updateAvailable_;
    update["latest"] = latestVersion_;
    update["url"] = updateUrl_;
  }
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
  printer["chamberLight"] = liveChamberLight_;

  JsonObject leds = doc["leds"].to<JsonObject>();
  auto putSlot = [&](const char* colorKey, const char* fxKey, const LightSlot& slot) {
    leds[colorKey] = colorHex(slot.r, slot.g, slot.b);
    leds[fxKey] = ledEffectName(slot.effect);
  };
  leds["brightness"] = cfg.brightness;
  leds["speed"] = cfg.speed;
  leds["finishedMinutes"] = cfg.finishedMinutes;
  leds["pin"] = cfg.ledPin;
  leds["count"] = cfg.ledCount;
  leds["maxCount"] = hw::kMaxNumLeds;
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
  leds["followChamberLight"] = cfg.followChamberLight;

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
  store_.data().hostname = hostname;
  store_.save();
  // Persist raw name; dhcpHostname() sanitizes for the wire after reboot.
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
      sendError(500, "udp-bind");
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
  int finishedMinutes = doc["finishedMinutes"].is<int>()
                            ? doc["finishedMinutes"].as<int>()
                            : store_.data().finishedMinutes;
  if (brightness < 0 || brightness > 100 || speed < 1 || speed > 100 ||
      finishedMinutes < led_cfg::kMinFinishedMinutes ||
      finishedMinutes > led_cfg::kMaxFinishedMinutes) {
    sendError(400, "bad-leds");
    return;
  }
  const bool hasPin = doc["pin"].is<int>();
  const bool hasCount = doc["count"].is<int>();
  int pin = hasPin ? doc["pin"].as<int>() : store_.data().ledPin;
  int count = hasCount ? doc["count"].as<int>() : store_.data().ledCount;
  if (hasPin && (pin < 0 || pin > 255 || !hw::isSupportedLedDataPin(static_cast<uint8_t>(pin)))) {
    sendError(400, "bad-led-pin");
    return;
  }
  if (hasCount && (count < 1 || count > static_cast<int>(hw::kMaxNumLeds))) {
    sendError(400, "bad-led-count");
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
  cfg.finishedMinutes = static_cast<uint8_t>(finishedMinutes);
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
  if (!doc["followChamberLight"].isNull()) {
    cfg.followChamberLight = doc["followChamberLight"].as<bool>();
  }
  const bool hwChanged =
      (hasPin && static_cast<uint8_t>(pin) != cfg.ledPin) ||
      (hasCount && static_cast<uint16_t>(count) != cfg.ledCount);
  if (hasPin) cfg.ledPin = static_cast<uint8_t>(pin);
  if (hasCount) cfg.ledCount = static_cast<uint16_t>(count);
  store_.save();
  if (hwChanged) {
    sendOk(true);
    delay(250);
    ESP.restart();
    return;
  }
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
    sendError(400, "bad-request");
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
    sendError(400, "bad-lang");
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
  if (otaBusy_) {
    sendError(409, "ota-busy");
    return;
  }
  otaBusy_ = true;
  prepareGithubHttps();
  const GithubReleaseInfo info = GithubOta::checkLatest();
  // finishBootUpdateCheck clears otaBusy_ and restores SoftAP.
  finishBootUpdateCheck(info.ok, info.ok && info.updateAvailable, info.latestVersion,
                         info.downloadUrl);
  JsonDocument doc;
  doc["ok"] = info.ok;
  doc["current"] = info.currentVersion;
  doc["latest"] = info.latestVersion;
  doc["available"] = info.updateAvailable;
  doc["url"] = info.downloadUrl;
  if (!info.error.isEmpty()) doc["error"] = info.error;
  String body;
  serializeJson(doc, body);
  sendJson(200, body);
}

void WebUi::handleUpdateGithub() {
  noteActivity();
  if (otaBusy_) {
    sendError(409, "ota-busy");
    return;
  }
  JsonDocument doc;
  if (!readJson(doc)) return;
  const char* url = doc["url"] | "";
  if (!url[0] || strncmp(url, "https://", 8) != 0) {
    sendError(400, "bad-url");
    return;
  }
  // Only allow GitHub download hosts.
  String urlStr = url;
  if (urlStr.indexOf("github.com/") < 0 && urlStr.indexOf("githubusercontent.com/") < 0) {
    sendError(400, "bad-url");
    return;
  }

  otaBusy_ = true;
  prepareGithubHttps();
  String error;
  const bool ok = GithubOta::installFromUrl(urlStr, error);
  if (!ok) {
    otaBusy_ = false;
    finishGithubHttps();
    sendError(500, error.c_str());
    return;
  }
  sendOk(true);
  delay(800);
  Serial.flush();
  ESP.restart();
}

void WebUi::handleNotFound() {
  String uri = impl_->server.uri();
  if (uri.startsWith("/api/")) {
    sendError(404, "not-found");
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
constexpr unsigned long kFrameIntervalMs = 20;  // ~50fps animation refresh
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
  ledController.setFinishedHoldMs(led_cfg::finishedHoldMs(cfg.finishedMinutes));
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

  web.begin();
  ledController.begin(web.settings().ledPin, web.settings().ledCount);
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
                     now - printerState.telemetry().finishedAtMs >= ledController.finishedHoldMs());
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
  // Optional: printer chamber_light drives the bar (one-way). Unknown = leave on.
  // Error still lights the bar so faults stay visible with the chamber light off.
  bool barOff = asleep;
  if (cfg.followChamberLight && printerState.haveChamberLight() && !printerState.chamberLightOn() &&
      shown != SystemState::Error) {
    barOff = true;
  }
  ledController.setAsleep(barOff);

  const char* reported = idleVisual ? "idle" : stateKey(shown);
  web.setLive(reported, printerState.telemetry().progressPercent, asleep && idleVisual, mqttUp);
  web.setChamberLight(printerState.haveChamberLight(), printerState.chamberLightOn());

  if (now - lastFrameMs >= kFrameIntervalMs) {
    lastFrameMs = now;
    ledController.render(shown, printerState.telemetry(), now);
  }
}
