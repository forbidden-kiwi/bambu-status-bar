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
#include <lwip/netdb.h>

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
