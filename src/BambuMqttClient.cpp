#include "BambuMqttClient.h"

#include "Config.h"

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
