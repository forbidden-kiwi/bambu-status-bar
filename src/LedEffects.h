#pragma once

#include <Arduino.h>
#include <cstdint>
#include <cstring>

// Selectable LED animations for Idle / Printing / Error.
enum class LedEffect : uint8_t {
  Pulse = 0,
  Solid = 1,
  Comet = 2,
  Wave = 3,
  Strobe = 4,
  Progress = 5,
  Rainbow = 6,
  Sparkle = 7,
  Fire = 8,
};

inline constexpr uint8_t kLedEffectCount = 9;

inline const char* ledEffectName(LedEffect effect) {
  switch (effect) {
    case LedEffect::Pulse:
      return "pulse";
    case LedEffect::Solid:
      return "solid";
    case LedEffect::Comet:
      return "comet";
    case LedEffect::Wave:
      return "wave";
    case LedEffect::Strobe:
      return "strobe";
    case LedEffect::Progress:
      return "progress";
    case LedEffect::Rainbow:
      return "rainbow";
    case LedEffect::Sparkle:
      return "sparkle";
    case LedEffect::Fire:
      return "fire";
  }
  return "pulse";
}

inline bool parseLedEffect(const char* text, LedEffect& out) {
  if (!text || !text[0]) return false;
  if (!strcmp(text, "pulse")) {
    out = LedEffect::Pulse;
    return true;
  }
  if (!strcmp(text, "solid")) {
    out = LedEffect::Solid;
    return true;
  }
  if (!strcmp(text, "comet")) {
    out = LedEffect::Comet;
    return true;
  }
  if (!strcmp(text, "wave")) {
    out = LedEffect::Wave;
    return true;
  }
  if (!strcmp(text, "strobe")) {
    out = LedEffect::Strobe;
    return true;
  }
  if (!strcmp(text, "progress")) {
    out = LedEffect::Progress;
    return true;
  }
  if (!strcmp(text, "rainbow")) {
    out = LedEffect::Rainbow;
    return true;
  }
  if (!strcmp(text, "sparkle")) {
    out = LedEffect::Sparkle;
    return true;
  }
  if (!strcmp(text, "fire")) {
    out = LedEffect::Fire;
    return true;
  }
  return false;
}

inline LedEffect clampLedEffect(uint8_t raw, LedEffect fallback) {
  if (raw >= kLedEffectCount) return fallback;
  return static_cast<LedEffect>(raw);
}
