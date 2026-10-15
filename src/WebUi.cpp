#include "WebUi.h"

#include "Config.h"
#include "GithubOta.h"
#include "NetHelpers.h"
#include "SsdpAdvertiser.h"
#include "WebPage.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

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
