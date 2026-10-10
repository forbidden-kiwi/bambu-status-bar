#pragma once

#include <Arduino.h>

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
