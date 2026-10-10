#include "GithubOta.h"

#include "Config.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFi.h>

#include <ArduinoJson.h>
#include <cstring>

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
