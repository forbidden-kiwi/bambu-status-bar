#include <Arduino.h>
#include <esp_ota_ops.h>

#include "BambuMqttClient.h"
#include "Config.h"
#include "LedController.h"
#include "PrinterState.h"
#include "WebUi.h"

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
