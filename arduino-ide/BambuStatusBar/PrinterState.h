#pragma once

#include <ArduinoJson.h>

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
  // Latched from print.lights_report; unknown until first chamber_light entry.
  bool haveChamberLight() const { return haveChamberLight_; }
  bool chamberLightOn() const { return chamberLightOn_; }

 private:
  void updateHeating(ArduinoJson::JsonVariantConst printObj);
  void updateChamberLight(ArduinoJson::JsonVariantConst printObj);
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
  bool haveChamberLight_ = false;
  bool chamberLightOn_ = true;

  float lastNozzleTemp_ = 0.0f;
  bool hasNozzleBaseline_ = false;
  bool isHeating_ = false;
  float heatingBaselineTemp_ = 0.0f;
  unsigned long lastHmsTimeMs_ = 0;
  // FINISH / stg_cur 255 stick on Bambu after a prior job. Only celebrate
  // Finished if we saw an active print in this MQTT session.
  bool sawActivePrint_ = false;
};
