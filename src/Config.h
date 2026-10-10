#pragma once

#include <cstdint>

// Product version shown in the web UI and compared against GitHub Releases.
constexpr const char* kFirmwareVersion = "1.0.5";

// Public GitHub repo used for over-the-air updates (no token on device).
constexpr const char* kGithubOwner = "forbidden-kiwi";
constexpr const char* kGithubRepo = "bambu-status-bar";
constexpr const char* kGithubAssetName = "firmware.bin";

constexpr const char* kDefaultHostname = "BambuStatusBar";
constexpr const char* kDefaultApSsid = "BambuStatusBar";
constexpr const char* kDefaultApPass = "987654321";
constexpr const char* kDefaultApIp = "192.168.254.1";

// Board pins come from the PlatformIO environment so the ESP32-C6 Super Mini
// (LED data on GPIO19, BOOT on GPIO9) and a classic ESP32 dev board can share
// this source. GPIO9 is the C6 strapping/BOOT pin: holding it during reset
// enters the ROM download mode, so the factory-reset gesture is a 3s hold
// while the firmware is already running.
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#if !defined(STATUS_LED_PIN) && defined(CONFIG_IDF_TARGET_ESP32C6)
#define STATUS_LED_PIN 19
#endif
#if !defined(STATUS_BOOT_PIN) && defined(CONFIG_IDF_TARGET_ESP32C6)
#define STATUS_BOOT_PIN 9
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 5
#endif
#ifndef STATUS_BOOT_PIN
#define STATUS_BOOT_PIN 0
#endif

namespace hw {
constexpr uint8_t kDefaultDataPin = STATUS_LED_PIN;
constexpr int kResetButtonPin = STATUS_BOOT_PIN;
constexpr uint16_t kDefaultNumLeds = 18;
constexpr uint16_t kMaxNumLeds = 64;
constexpr uint16_t kMaxMilliwatts = 850;
constexpr unsigned long kResetHoldMs = 3000;

// FastLED needs the data pin as a compile-time template argument, so only this
// chip-valid set can be selected from the web UI.
inline bool isSupportedLedDataPin(uint8_t pin) {
  switch (pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    // FastLED marks GPIO12/13 (USB) and several others unusable on C6.
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 14:
    case 15:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
#elif defined(CONFIG_IDF_TARGET_ESP32)
    case 0:
    case 2:
    case 4:
    case 5:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 21:
    case 22:
    case 23:
    case 25:
    case 26:
    case 27:
    case 32:
    case 33:
#else
    case STATUS_LED_PIN:
#endif
      return true;
    default:
      return false;
  }
}
}  // namespace hw

namespace led_cfg {
// Solid finished colour for this long, then fall back to the idle effect.
constexpr unsigned long kFinishedSolidMs = 15UL * 60UL * 1000UL;
// Idle longer than this turns the bar off until the UI or a print wakes it.
constexpr unsigned long kStandbyAfterIdleMs = 120UL * 60UL * 1000UL;
constexpr uint8_t kDefaultBrightnessPercent = 50;
constexpr uint8_t kDefaultSpeedPercent = 50;
}  // namespace led_cfg

namespace ota_cfg {
// Let DHCP/DNS settle after STA connect before the boot GitHub check.
constexpr unsigned long kBootCheckDelayMs = 8000;
}  // namespace ota_cfg

namespace mqtt_cfg {
constexpr int kPort = 8883;
constexpr const char* kUsername = "bblp";
// First reconnect gap; BambuMqttClient stretches this after failures.
constexpr unsigned long kReconnectIntervalMs = 1500;
// Re-request full status if the printer stays quiet after connect.
constexpr unsigned long kPushallIntervalMs = 45000;
// Periodic pushall while connected so stale stg_cur cannot latch.
constexpr unsigned long kPeriodicPushallMs = 60000;
}  // namespace mqtt_cfg
