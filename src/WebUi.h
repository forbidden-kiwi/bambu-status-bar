#pragma once

#include "Settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

// Local web UI: setup pages plus file and GitHub OTA.
// The AP stays up whenever Wi-Fi is down so a bad password cannot lock the device out.
class WebUi {
 public:
  void begin();
  void loop();

  void setLive(const char* state, uint8_t progress, bool asleep, bool mqttConnected);
  void setChamberLight(bool known, bool on);
  void onOtaStart(std::function<void()> cb) { onOtaStart_ = std::move(cb); }

  void factoryReset();
  bool consumeActivity();
  bool staConnected() const { return staConnected_; }
  bool printerReady() const { return store_.printerReady(); }
  bool otaBusy() const { return otaBusy_; }
  uint32_t revision() const { return store_.revision(); }
  const DeviceSettings& settings() const { return store_.data(); }

 private:
  struct FoundNet {
    String ssid;
    int32_t rssi;
    bool secure;
  };
  struct FoundPrinter {
    String ip;
    String serial;
    String name;
  };

  void applyHostname();
  void pushHostnameToDhcp();
  String dhcpHostname() const;
  void startAp();
  void stopAp();
  void syncAp(bool staUp);
  void syncCaptiveDns(bool staUp);
  void prepareGithubHttps();
  void pollBootUpdateCheck();
  void finishBootUpdateCheck(bool ok, bool available, const String& latest, const String& url);
  static void bootUpdateCheckTask(void* arg);
  void startMdns();
  void startSsdp();
  void stopSsdp();
  void pollWifiConnect();
  void pollWifiScan();
  void pollPrinterScan();
  void notePrinterDatagram(const uint8_t* data, int len);
  String deviceUuid() const;

  void noteActivity() { activity_ = true; }
  void sendJson(int code, const String& body);
  void sendError(int code, const char* key);
  void sendOk(bool reboot);
  bool readJson(ArduinoJson::JsonDocument& doc);
  void sendPage();

  void handleStatus();
  void handleWifiScanGet();
  void handleWifiScanPost();
  void handleWifiConnect();
  void handleHostname();
  void handlePrinterScanGet();
  void handlePrinterScanPost();
  void handlePrinterSave();
  void handleLeds();
  void handleLedsReset();
  void handleAp();
  void handleLanguage();
  void handleFactory();
  void handleUpdateCheck();
  void handleUpdateGithub();
  void handleNotFound();

  SettingsStore store_;
  bool staConnected_ = false;
  bool apRunning_ = false;
  int apChannel_ = 1;
  bool mdnsStarted_ = false;
  unsigned long mdnsRetryAt_ = 0;

  bool channelAligned_ = false;
  bool connectArmed_ = false;
  bool connectPending_ = false;
  bool connectFailed_ = false;
  bool connectSawDrop_ = false;
  unsigned long connectArmAt_ = 0;
  unsigned long connectStarted_ = 0;
  String pendingSsid_;
  String pendingPass_;

  bool wifiScanRunning_ = false;
  bool printerScanRunning_ = false;
  unsigned long printerScanDeadline_ = 0;
  bool hostnamePushed_ = false;
  unsigned long staLostSinceMs_ = 0;

  bool activity_ = false;
  bool otaBusy_ = false;
  bool otaReject_ = false;
  std::function<void()> onOtaStart_;

  // One GitHub release check after STA comes up (shown in the hero next to version).
  bool bootUpdateCheckDone_ = false;
  bool bootUpdateCheckRunning_ = false;
  bool updateAvailable_ = false;
  unsigned long staUpSinceMs_ = 0;
  String latestVersion_;
  String updateUrl_;

  const char* liveState_ = "provisioning";
  uint8_t liveProgress_ = 0;
  bool liveAsleep_ = false;
  bool liveMqtt_ = false;
  const char* liveChamberLight_ = "unknown";  // on | off | unknown

  // Containers live in the .cpp so this header stays free of WebServer/UDP includes.
  struct Impl;
  Impl* impl_ = nullptr;
};
