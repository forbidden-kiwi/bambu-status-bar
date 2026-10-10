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
`src/`). After editing `src/`, regenerate:

```bash
python tools/generate_arduino_sketch.py
```

## First setup

1. Join the hotspot `BambuStatusBar` (password `987654321` unless changed).
2. Open `http://192.168.254.1`
3. Connect home Wi‑Fi, bind printer IP / serial / access code.
4. Open the device on your LAN (hostname `BambuStatusBar` or its IP).
