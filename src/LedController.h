#pragma once

#include <FastLED.h>

#include "Config.h"
#include "LedEffects.h"
#include "PrinterState.h"

struct LedStyleSlot {
  CRGB color;
  LedEffect effect;
};

struct LedStyle {
  uint8_t brightness = led_cfg::kDefaultBrightnessPercent;  // 0..100
  uint8_t speed = 50;                                       // 1..100, 50 is 1x
  LedStyleSlot idle{CRGB::White, LedEffect::Pulse};
  LedStyleSlot printing{CRGB::White, LedEffect::Progress};
  LedStyleSlot error{CRGB::Red, LedEffect::Strobe};
  LedStyleSlot homing{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot leveling{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot preparing{CRGB(255, 120, 0), LedEffect::Comet};
  LedStyleSlot heating{CRGB(255, 80, 0), LedEffect::Progress};
  LedStyleSlot downloading{CRGB(255, 220, 0), LedEffect::Comet};
  LedStyleSlot paused{CRGB::White, LedEffect::Pulse};
  LedStyleSlot finished{CRGB::Green, LedEffect::Solid};
  bool reverse = false;  // flip physical LED order for mount orientation
};

class LedController {
 public:
  void begin();
  void setStyle(const LedStyle& style);
  void setAsleep(bool asleep) { asleep_ = asleep; }
  void render(SystemState state, const PrinterTelemetry& telemetry, unsigned long nowMs);

 private:
  void renderProgressBar(CRGB onColor, CRGB offColor, uint8_t percent);
  void renderComet(CRGB color, unsigned long nowMs, uint16_t stepMs);
  void renderWave(unsigned long nowMs, uint8_t hue);
  void renderWaveColor(CRGB color, unsigned long nowMs);
  void renderPulse(CRGB color, unsigned long nowMs, uint16_t periodMs);
  void renderStrobe(CRGB color, unsigned long nowMs, uint16_t onMs, uint16_t offMs);
  void renderSolid(CRGB color);
  void renderRainbow(unsigned long nowMs);
  void renderSparkle(CRGB color, unsigned long nowMs);
  void renderFire(unsigned long nowMs);
  void renderHeating(uint8_t percent);
  void renderEffect(LedEffect effect, CRGB color, uint8_t progressPercent, unsigned long nowMs);
  void renderSlot(const LedStyleSlot& slot, uint8_t progressPercent, unsigned long nowMs);
  void showBlack();
  void showLeds();
  uint16_t scaled(uint16_t baseMs) const;

  CRGB leds_[hw::kNumLeds];
  LedStyle style_;
  bool asleep_ = false;
  uint8_t heat_[hw::kNumLeds] = {};
};
