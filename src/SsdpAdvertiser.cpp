#include "SsdpAdvertiser.h"

#include "Config.h"

#include <WiFi.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t kSsdpPort = 1900;
constexpr uint8_t kSsdpTtl = 2;
constexpr unsigned long kCacheSeconds = 1800;
constexpr unsigned long kNotifyFastMs = 60UL * 1000UL;
constexpr unsigned long kNotifySlowMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kNotifyFastWindowMs = 5UL * 60UL * 1000UL;
const IPAddress kSsdpMulticast(239, 255, 255, 250);

bool wantsRoot(const String& st) {
  return st.equalsIgnoreCase("ssdp:all") || st.equalsIgnoreCase("upnp:rootdevice") ||
         st.equalsIgnoreCase("urn:schemas-upnp-org:device:Basic:1");
}

String headerLine(const String& text, const char* key) {
  String lower = text;
  lower.toLowerCase();
  String needle(key);
  needle.toLowerCase();
  needle += ":";
  int found = lower.indexOf(needle);
  if (found < 0) return "";
  if (found > 0 && lower[found - 1] != '\n') return "";
  int colon = found + needle.length() - 1;
  int end = text.indexOf('\n', colon);
  if (end < 0) end = text.length();
  String value = text.substring(colon + 1, end);
  value.trim();
  value.replace("\r", "");
  return value;
}
}  // namespace

void SsdpAdvertiser::configure(const String& friendlyName, const String& uuid) {
  friendlyName_ = friendlyName.length() ? friendlyName : String("BambuStatusBar");
  uuid_ = uuid;
  if (!uuid_.isEmpty() && !uuid_.startsWith("uuid:")) uuid_ = String("uuid:") + uuid_;
}

void SsdpAdvertiser::begin(const String& friendlyName, const String& uuid) {
  stop();
  configure(friendlyName, uuid);
  if (uuid_.isEmpty()) return;

  // TTL=2 matches ESP32SSDP; WiFiUDP multicast often fails to be noticed by AVM.
  if (!udp_.listenMulticast(kSsdpMulticast, kSsdpPort, kSsdpTtl)) {
    Serial.println(F("SSDP multicast join failed"));
    return;
  }
  udp_.onPacket([this](AsyncUDPPacket packet) { onPacket(packet); });
  running_ = true;
  startedMs_ = millis();
  nextNotifyMs_ = 0;
  sendNotifyAll();
  Serial.print(F("SSDP advertising "));
  Serial.println(locationUrl());
}

void SsdpAdvertiser::stop() {
  running_ = false;
  udp_.close();
}

void SsdpAdvertiser::setFriendlyName(const String& friendlyName) {
  if (friendlyName.length()) friendlyName_ = friendlyName;
}

void SsdpAdvertiser::loop() {
  if (!running_) return;
  if (WiFi.status() != WL_CONNECTED) {
    stop();
    return;
  }
  if (static_cast<long>(millis() - nextNotifyMs_) >= 0) sendNotifyAll();
}

String SsdpAdvertiser::locationUrl() const {
  return String("http://") + WiFi.localIP().toString() + "/description.xml";
}

String SsdpAdvertiser::usn(const char* st) const {
  String out = uuid_;
  if (st && st[0]) {
    out += "::";
    out += st;
  }
  return out;
}

String SsdpAdvertiser::schemaXml() const {
  // Relative presentationURL (ESP32SSDP style); Fritzbox joins it with URLBase.
  const String name = friendlyName_.length() ? friendlyName_ : String("BambuStatusBar");
  const String udn = uuid_.length() ? uuid_ : String("uuid:00000000-0000-0000-0000-000000000000");
  const String ip = WiFi.localIP().toString();
  const String base = String("http://") + ip + ":80/";
  String xml;
  xml.reserve(900);
  xml += F("<?xml version=\"1.0\"?>\r\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
           "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n");
  xml += F("<URLBase>");
  xml += base;
  xml += F("</URLBase>\r\n<device>\r\n"
           "<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\r\n"
           "<friendlyName>");
  xml += name;
  xml += F("</friendlyName>\r\n"
           "<manufacturer>Bambu Status Bar</manufacturer>\r\n"
           "<manufacturerURL>https://github.com/");
  xml += kGithubOwner;
  xml += '/';
  xml += kGithubRepo;
  xml += F("</manufacturerURL>\r\n"
           "<modelDescription>LED status bar for Bambu Lab printers</modelDescription>\r\n"
           "<modelName>Bambu Status Bar</modelName>\r\n"
           "<modelNumber>");
  xml += kFirmwareVersion;
  xml += F("</modelNumber>\r\n"
           "<modelURL>https://github.com/");
  xml += kGithubOwner;
  xml += '/';
  xml += kGithubRepo;
  xml += F("</modelURL>\r\n"
           "<serialNumber>");
  xml += WiFi.macAddress();
  xml += F("</serialNumber>\r\n"
           "<UDN>");
  xml += udn;
  xml += F("</UDN>\r\n"
           "<presentationURL>/</presentationURL>\r\n"
           "</device>\r\n</root>\r\n");
  return xml;
}

void SsdpAdvertiser::sendNotifyAll() {
  if (!running_ || WiFi.status() != WL_CONNECTED) return;
  // ESP32SSDP / Windows / AVM expect root + uuid + device-type alive signals.
  sendNotify("upnp:rootdevice");
  sendNotify(uuid_.c_str());
  sendNotify("urn:schemas-upnp-org:device:Basic:1");
  const unsigned long age = millis() - startedMs_;
  nextNotifyMs_ = millis() + (age < kNotifyFastWindowMs ? kNotifyFastMs : kNotifySlowMs);
}

void SsdpAdvertiser::sendNotify(const char* nt) {
  if (!nt || !nt[0]) return;
  const String loc = locationUrl();
  const String ntsUsn = (strncmp(nt, "uuid:", 5) == 0) ? String(nt) : usn(nt);
  char buf[640];
  snprintf(buf, sizeof(buf),
           "NOTIFY * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "CACHE-CONTROL: max-age=%lu\r\n"
           "LOCATION: %s\r\n"
           "NT: %s\r\n"
           "NTS: ssdp:alive\r\n"
           "SERVER: Arduino/1.0 UPnP/1.1 BambuStatusBar/%s\r\n"
           "USN: %s\r\n"
           "\r\n",
           static_cast<unsigned long>(kCacheSeconds), loc.c_str(), nt, kFirmwareVersion,
           ntsUsn.c_str());
  udp_.writeTo(reinterpret_cast<const uint8_t*>(buf), strlen(buf), kSsdpMulticast, kSsdpPort);
}

void SsdpAdvertiser::sendSearchResponse(const IPAddress& addr, uint16_t port, const char* st) {
  if (!running_ || WiFi.status() != WL_CONNECTED) return;
  const String loc = locationUrl();
  const String responseSt = st && st[0] ? String(st) : String("upnp:rootdevice");
  const String responseUsn =
      responseSt.startsWith("uuid:") ? responseSt : usn(responseSt.c_str());
  char buf[640];
  snprintf(buf, sizeof(buf),
           "HTTP/1.1 200 OK\r\n"
           "CACHE-CONTROL: max-age=%lu\r\n"
           "EXT:\r\n"
           "LOCATION: %s\r\n"
           "SERVER: Arduino/1.0 UPnP/1.1 BambuStatusBar/%s\r\n"
           "ST: %s\r\n"
           "USN: %s\r\n"
           "\r\n",
           static_cast<unsigned long>(kCacheSeconds), loc.c_str(), kFirmwareVersion,
           responseSt.c_str(), responseUsn.c_str());
  udp_.writeTo(reinterpret_cast<const uint8_t*>(buf), strlen(buf), addr, port);
}

void SsdpAdvertiser::onPacket(AsyncUDPPacket& packet) {
  if (!running_ || packet.length() == 0) return;
  const size_t len = packet.length() < 512 ? packet.length() : 512;
  char raw[513];
  memcpy(raw, packet.data(), len);
  raw[len] = 0;
  String text(raw);
  if (text.indexOf("M-SEARCH") < 0) return;
  String st = headerLine(text, "ST");
  if (st.isEmpty()) return;
  if (st.equalsIgnoreCase(uuid_)) {
    sendSearchResponse(packet.remoteIP(), packet.remotePort(), st.c_str());
    return;
  }
  if (!wantsRoot(st)) return;
  const char* replySt = st.equalsIgnoreCase("ssdp:all") ? "upnp:rootdevice" : st.c_str();
  sendSearchResponse(packet.remoteIP(), packet.remotePort(), replySt);
}
