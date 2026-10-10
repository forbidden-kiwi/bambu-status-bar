#pragma once

// Product version shown in the web UI and compared against GitHub Releases.
constexpr const char* kFirmwareVersion = "1.0.0";

// Public GitHub repo used for over-the-air updates (no token on device).
constexpr const char* kGithubOwner = "forbidden-kiwi";
constexpr const char* kGithubRepo = "bambu-status-bar";
constexpr const char* kGithubAssetName = "firmware.bin";

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
constexpr int kDataPin = STATUS_LED_PIN;
constexpr int kResetButtonPin = STATUS_BOOT_PIN;
constexpr int kNumLeds = 18;
constexpr uint16_t kMaxMilliwatts = 850;
constexpr unsigned long kResetHoldMs = 3000;
}  // namespace hw

namespace led_cfg {
// Solid finished colour for this long, then fall back to the idle effect.
constexpr unsigned long kFinishedSolidMs = 15UL * 60UL * 1000UL;
// Idle longer than this turns the bar off until the UI or a print wakes it.
constexpr unsigned long kStandbyAfterIdleMs = 120UL * 60UL * 1000UL;
constexpr uint8_t kDefaultBrightnessPercent = 50;
}  // namespace led_cfg

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
