#include "GithubOta.h"

#include "Config.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* kUserAgent = "BambuStatusBar-OTA";
// ESP-IDF xTaskCreate stack size is in bytes. mbedTLS needs far more than the
// WebServer request path provides on the Arduino loop task.
constexpr uint32_t kTlsTaskStackBytes = 24576;
constexpr uint32_t kCheckTimeoutMs = 45000;
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

void prepareSecureClient(WiFiClientSecure& client, uint32_t handshakeSec, uint32_t timeoutMs) {
  client.setInsecure();
  client.setHandshakeTimeout(handshakeSec);
  client.setTimeout(timeoutMs);
}

bool httpGetToString(const String& url, String& body, String& errorOut) {
  WiFiClientSecure client;
  prepareSecureClient(client, 20, 25000);

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(25000);
  if (!http.begin(client, url)) {
    errorOut = "http-begin";
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setHttpError(errorOut, http, code);
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return true;
}

GithubReleaseInfo checkLatestImpl() {
  GithubReleaseInfo info;
  info.currentVersion = kFirmwareVersion;

  if (WiFi.status() != WL_CONNECTED) {
    info.error = "no-wifi";
    return info;
  }

  // Fail fast with a clear log if DNS is broken (captive portal / AP+STA).
  IPAddress githubIp;
  if (!WiFi.hostByName("api.github.com", githubIp)) {
    Serial.println(F("OTA DNS api.github.com failed"));
    info.error = "http-connect";
    return info;
  }
  Serial.print(F("OTA DNS api.github.com -> "));
  Serial.println(githubIp);

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

bool installFromUrlImpl(const String& downloadUrl, String& errorOut) {
  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "no-wifi";
    return false;
  }
  if (downloadUrl.isEmpty()) {
    errorOut = "no-url";
    return false;
  }

  WiFiClientSecure client;
  prepareSecureClient(client, 30, 60000);

  HTTPClient http;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setTimeout(60000);
  if (!http.begin(client, downloadUrl)) {
    errorOut = "http-begin";
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setHttpError(errorOut, http, code);
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

// Shared job state lives in BSS so a timed-out waiter cannot free stack under the task.
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
    vTaskDelay(pdMS_TO_TICKS(500 * (attempt + 1)));
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
    // Fall back to the caller stack if the task cannot be created.
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
