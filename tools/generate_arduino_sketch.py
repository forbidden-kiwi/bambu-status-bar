#!/usr/bin/env python3
"""Mirrors firmware/src into an Arduino IDE sketch folder.

PlatformIO (platformio.ini) is the source of truth. This script copies every
header into arduino-ide/BambuStatusBar/ and concatenates the .cpp files into
BambuStatusBar.ino. Headers stay as real #includes so Arduino's automatic
function prototypes are inserted *after* types like enum class LedEffect
(inlining headers into the .ino breaks that ordering).

Usage:
    python3 tools/generate_arduino_sketch.py [--check]

--check: don't write, exit non-zero if the mirror would differ (CI).
"""
import argparse
import re
import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
SRC_DIR = FIRMWARE_DIR / "src"
SKETCH_DIR = FIRMWARE_DIR / "arduino-ide" / "BambuStatusBar"
OUTPUT = SKETCH_DIR / "BambuStatusBar.ino"

# Dependency order for headers (included at the top of the .ino) and for the
# concatenated .cpp bodies that follow.
FILE_ORDER = [
    "Config.h",
    "NetHelpers.h",
    "PrinterState.h",
    "PrinterState.cpp",
    "LedEffects.h",
    "LedController.h",
    "LedController.cpp",
    "BambuMqttClient.h",
    "BambuMqttClient.cpp",
    "Settings.h",
    "Settings.cpp",
    "GithubOta.h",
    "GithubOta.cpp",
    "SsdpAdvertiser.h",
    "SsdpAdvertiser.cpp",
    "WebPage.h",
    "WebUi.h",
    "WebUi.cpp",
    "main.cpp",
]

HEADERS = [name for name in FILE_ORDER if name.endswith(".h")]
SOURCES = [name for name in FILE_ORDER if name.endswith(".cpp")]

PRAGMA_ONCE_RE = re.compile(r"^\s*#pragma once\s*$")
LOCAL_INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$')
LIBRARY_INCLUDE_RE = re.compile(r"^\s*#include\s+<([^>]+)>\s*$")


def collect_library_includes() -> list[str]:
    library_includes: list[str] = []
    for filename in FILE_ORDER:
        for line in (SRC_DIR / filename).read_text(encoding="utf-8").splitlines():
            lib_match = LIBRARY_INCLUDE_RE.match(line)
            if lib_match and lib_match.group(1) not in library_includes:
                library_includes.append(lib_match.group(1))
    return library_includes


def source_body(filename: str, library_includes: list[str]) -> str:
    body_lines = []
    for line in (SRC_DIR / filename).read_text(encoding="utf-8").splitlines():
        if PRAGMA_ONCE_RE.match(line):
            continue
        if LOCAL_INCLUDE_RE.match(line):
            continue  # provided by the sketch-level #include "….h" block
        lib_match = LIBRARY_INCLUDE_RE.match(line)
        if lib_match:
            if lib_match.group(1) not in library_includes:
                library_includes.append(lib_match.group(1))
            continue
        body_lines.append(line)
    section = "\n".join(body_lines).strip("\n")
    return f"// ==== {filename} ====\n{section}\n"


def generate_ino() -> str:
    library_includes = collect_library_includes()
    body_sections = [source_body(name, library_includes) for name in SOURCES]

    header = (
        "// GENERATED FILE — do not edit directly.\n"
        "// Source of truth is src/*.{h,cpp}; regenerate with:\n"
        "//   python3 tools/generate_arduino_sketch.py\n"
        "//\n"
        "// Headers live beside this .ino (copied from src/) so Arduino IDE\n"
        "// function prototypes are emitted after enum/class declarations.\n\n"
    )
    lib_block = "\n".join(f"#include <{name}>" for name in library_includes)
    local_block = "\n".join(f'#include "{name}"' for name in HEADERS)
    return header + lib_block + "\n\n" + local_block + "\n\n" + "\n".join(body_sections)


def expected_headers() -> dict[str, str]:
    return {
        name: (SRC_DIR / name).read_text(encoding="utf-8")
        for name in HEADERS
    }


def write_mirror() -> None:
    SKETCH_DIR.mkdir(parents=True, exist_ok=True)
    for name, text in expected_headers().items():
        (SKETCH_DIR / name).write_text(text, encoding="utf-8", newline="\n")
    OUTPUT.write_text(generate_ino(), encoding="utf-8", newline="\n")
    print(f"Wrote {OUTPUT}")
    print(f"Wrote {len(HEADERS)} headers into {SKETCH_DIR}")


def check_mirror() -> int:
    generated = generate_ino()
    current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
    stale = []
    if current != generated:
        stale.append(str(OUTPUT))
    for name, text in expected_headers().items():
        path = SKETCH_DIR / name
        existing = path.read_text(encoding="utf-8") if path.exists() else ""
        if existing != text:
            stale.append(str(path))
    if stale:
        print("Arduino sketch mirror is out of date:")
        for path in stale:
            print(f"  - {path}")
        print("Run: python3 tools/generate_arduino_sketch.py")
        return 1
    print("Arduino sketch mirror is up to date.")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        sys.exit(check_mirror())
    write_mirror()


if __name__ == "__main__":
    main()
