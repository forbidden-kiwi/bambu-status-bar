#include "PrinterState.h"

#include <cstring>

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
