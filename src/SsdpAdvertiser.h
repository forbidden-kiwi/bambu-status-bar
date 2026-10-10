#pragma once

#include <Arduino.h>
#include <AsyncUDP.h>

// UPnP/SSDP advertiser (ESP32SSDP-style) so Fritzbox can store presentationURL
// and show a clickable web-UI link. PrintSphere/PandaStatusOS do not advertise
// over SSDP; they rely on DHCP hostname + mDNS. This is an extra signal for AVM.
class SsdpAdvertiser {
 public:
  void configure(const String& friendlyName, const String& uuid);
  void begin(const String& friendlyName, const String& uuid);
  void stop();
  void loop();
  void setFriendlyName(const String& friendlyName);
  bool running() const { return running_; }
  String schemaXml() const;

 private:
  void sendNotifyAll();
  void sendNotify(const char* nt);
  void sendSearchResponse(const IPAddress& addr, uint16_t port, const char* st);
  void onPacket(AsyncUDPPacket& packet);
  String locationUrl() const;
  String usn(const char* st) const;

  AsyncUDP udp_;
  bool running_ = false;
  String friendlyName_;
  String uuid_;
  unsigned long nextNotifyMs_ = 0;
  unsigned long startedMs_ = 0;
};
