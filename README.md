# Bambu Status Bar

ESP32-C6 firmware that listens to a Bambu Lab printer over local MQTT and
drives a WS2812 LED bar for print status — idle, homing, heating, progress,
pause, finish, and errors.

Works with LAN Mode / Developer Mode printers (A1, P1, X1, H2 families, …).

## Features

- Local MQTT status (TLS port 8883)
- Web setup UI (Wi‑Fi, printer, LED colours & animations)
- File OTA and **GitHub Release OTA** (public repo)
- Setup hotspot fallback when Wi‑Fi is down

## Build (PlatformIO)

```bash
pio run -e esp32c6
pio run -e esp32c6 -t upload
```

OTA image: `.pio/build/esp32c6/firmware.bin`  
First flash / recovery: `.pio/build/esp32c6/firmware.factory.bin`

## GitHub updates

Releases are published from this repository. On the device web UI:

1. **Firmware Update → Check for update**
2. If a newer tag is available → **Install update**

Release workflow: push a tag like `v1.0.1` (or run the *Release firmware*
Action manually). The Action builds `esp32c6` and attaches `firmware.bin`.

Configured repo in firmware: `forbidden-kiwi/bambu-status-bar`

## Arduino IDE

Open `arduino-ide/BambuStatusBar/BambuStatusBar.ino` (generated mirror of
`src/`, with headers copied beside the sketch). After editing `src/`, regenerate:

```bash
python tools/generate_arduino_sketch.py
```

### Required Tools settings (ESP32-C6)

The sketch is larger than the default **1.2 MB** app partition. Use an OTA
scheme with ~1.9 MB app slots (same idea as PlatformIO `partitions_4mb_ota.csv`).

| Setting | Value |
| --- | --- |
| Board | `ESP32C6 Dev Module` |
| Flash Size | `4MB (32Mb)` |
| Partition Scheme | **`Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)`** |
| Upload Speed | `921600` (or lower if uploads fail) |

Do **not** use `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)` — the linker
will fail with “text section exceeds available space” (~111% of 1.2 MB).

`No FS 4MB (2MB APP x2)` also fits and supports OTA; prefer **Minimal SPIFFS**
above so the menu name matches common Arduino-ESP32 cores.

Also install the **ESP32** board package (Espressif) via Board Manager, and these
Library Manager packages: **PubSubClient**, **FastLED**, **ArduinoJson**.

## First setup

1. Join the hotspot `BambuStatusBar` (password `987654321` unless changed).
2. Open `http://192.168.254.1`
3. Connect home Wi‑Fi, bind printer IP / serial / access code.
4. Open the device on your LAN (hostname `BambuStatusBar` or its IP).
